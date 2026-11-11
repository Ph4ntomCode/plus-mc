// The plugin host: discovery, loading, lifecycle and dispatch for the native
// plugin API (docs/PLUGIN_API.md, docs/PLAN.md §9.2).
//
// The ABI a plugin compiles against is `pluginapi/plugin.h`; this header is the
// engine side. A server owns one Host (`mc::plugins::host()`), loads a
// directory of plugins into it, pumps its scheduler from the tick loop, and
// routes the session-side interactions through it:
//
//   * a chat line, before it is relayed (`fire_player_chat`);
//   * a player joining or leaving (`fire_player_join` / `fire_player_leave`);
//   * a block placement or a completed dig, before the write
//     (`fire_block_place` / `fire_block_break`);
//   * a bucket or flint-and-steel use on a block, before its effect
//     (`fire_item_use_on`);
//   * a command line no built-in accepted (`dispatch_command`).
//
// Nothing here throws: a plugin that fails to load is logged and skipped, and
// an exception escaping a plugin callback is caught, logged and contained.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pluginapi/plugin.h"

namespace mc::plugins {

// What the engine answers a plugin's `player_get` with. The host copies these
// fields into the plugin's `pmc_player`, so a lookup can hand back a local
// without worrying about lifetime.
struct PlayerView {
  std::string name;
  double x = 0;
  double y = 0;
  double z = 0;
  float yaw = 0;
  float pitch = 0;
  int gamemode = 0;
  float health = 20.0f;
  int32_t entity_id = 0;
  // The same observed stance delta `EntityView` carries; see `pmc_player`.
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
};

// The engine-side lookups behind `world_block` / `world_biome`. `state` is the
// canonical name plus its sorted property list; `biome` may be left empty when
// the engine can answer only the block. Returning false means "no world" and
// reaches the plugin as -1.
using PlayerLookup = std::function<bool(const std::string& name, PlayerView* out)>;
using BlockLookup =
    std::function<bool(int32_t x, int32_t y, int32_t z, std::string* state, std::string* biome)>;
// The engine-side write behind `world_set_block`. `state` is the plugin's
// canonical state name; the engine validates it, writes through its own
// block-change path, and answers one of the ABI's `pmc_block_result` values.
using BlockWriter = std::function<int(int32_t x, int32_t y, int32_t z, const std::string& state)>;

// One player block interaction, as the session reports it to the block events.
// The strings are built before the event is fired and copied into the ABI's
// pointers only for the duration of the call.
struct BlockEvent {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  std::string block;
  std::string previous;
  int32_t block_id = -1;
};

// One player item use on a block, as the session reports it to
// `PMC_EVENT_ITEM_USE_ON`. The string is the canonical item name; the clicked
// cell's state and id sit beside it, because that is what the handler logs
// and what a cancelled use restores.
struct ItemUseEvent {
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  int32_t face = 0;
  std::string item;
  std::string block;
  int32_t block_id = -1;
};

// One mob death, as the world tick (or the killer's session) reports it to
// `PMC_EVENT_ENTITY_DEATH`. `type` is the registry name; `killer_name` is empty
// when nothing was credited. The host copies both into the ABI's pointers only
// for the duration of the call.
struct EntityDeathEvent {
  int32_t entity_id = 0;
  std::string type;
  double x = 0, y = 0, z = 0;
  int32_t killer_entity_id = -1;
  std::string killer_name;
};

// One live entity, as the engine's registries answer `entities`. Copied under
// each registry's lock, so the host only ever sees a value it owns.
struct EntityView {
  int32_t entity_id = 0;
  int32_t type_id = 0;
  std::string type;
  std::string name;
  double x = 0;
  double y = 0;
  double z = 0;
  float health = 0.0f;
  int32_t age = 0;
  // Delta movement in blocks per tick and look angles in degrees. The engine
  // fills what the kind tracks; see `pmc_entity`'s comment for the gaps.
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
  float yaw = 0.0f;
  float pitch = 0.0f;
};

// The engine-side entity listing behind `entities`. Returning false means "no
// world" and reaches the plugin as -1. The callback visits on the calling
// thread, in the order the registries provided; the host holds no lock while a
// plugin callback runs.
using EntityLookup = std::function<bool(std::vector<EntityView>* out)>;

// The engine-side single-entity read behind `entity_get`. It answers one of the
// ABI's `pmc_entity_result` values; unset answers
// `PMC_ENTITY_UNSUPPORTED`, because a host with no engine behind it has no
// entity to find.
using EntityGetter = std::function<int(int32_t entity_id, EntityView* out)>;

// The engine-side by-name read behind `entity_find`. It answers an
// `enum pmc_entity_result` value and fills one snapshot; unset answers
// `PMC_ENTITY_UNSUPPORTED`.
using EntityFinder = std::function<int(const std::string& name, EntityView* out)>;

// The engine-side writes behind `entity_despawn`, `entity_damage` and
// `entity_teleport`. Each answers one of the ABI's `pmc_entity_result` values;
// unset writes answer `PMC_ENTITY_UNSUPPORTED`, because a host with no engine
// behind it has no entity to change.
using EntityDespawner = std::function<int(int32_t entity_id)>;
using EntityDamager = std::function<int(int32_t entity_id, float amount)>;
using EntityTeleporter = std::function<int(int32_t entity_id, double x, double y, double z)>;

// One player-inventory stack, as the engine answers `inventory_get`.
struct ItemView {
  int32_t item = 0;
  int32_t count = 0;
  int32_t damage = 0;
  int32_t max_stack_size = 0;
  std::string name;
};

// The read and write halves behind `inventory_get` / `inventory_set`. Both
// answer one of the ABI's `pmc_inventory_result` values; the writer validates
// the item name and count itself.
using InventoryReader = std::function<int(const std::string& player, int slot, ItemView* out)>;
using InventoryWriter =
    std::function<int(const std::string& player, int slot, const std::string& item, int count)>;

// The engine's ops.json view: a player name (case-insensitive) answers their
// operator level, or -1 when they are not listed. Unset makes every plugin
// command level 0 and `player_op_level` answer -1.
using OpLevelLookup = std::function<int(const std::string& player)>;

// The engine's world clock and weather behind `world_time` / `world_set_time` /
// `world_weather` / `world_set_weather`. The readers answer -1 when no world is
// registered; the writers report an `enum pmc_world_result` value. All four run
// on the plugin handler's thread and the engine side owns its synchronisation
// (the clock is atomic, the weather has its own lock).
using WorldTime = std::function<int64_t()>;
using WorldTimeSetter = std::function<int(int64_t ticks)>;
using WeatherReader = std::function<int()>;
using WeatherSetter = std::function<int(int weather, int32_t duration_ticks)>;

// The engine's chat delivery behind `player_message` / `broadcast` /
// `player_count`. The writers answer an `enum pmc_message_result` value (the
// broadcast's answer is its recipient count), and the counter answers the
// number in play. All three run on the plugin handler's thread and the engine
// side owns its synchronisation (the player registry has its own lock).
using PlayerMessenger = std::function<int(const std::string& player, const std::string& message)>;
using Broadcaster = std::function<int(const std::string& message)>;
using PlayerCounter = std::function<int()>;

// The engine's player-state writes behind `player_set_gamemode`,
// `player_set_health` and `player_set_food`. Each answers an
// `enum pmc_player_result` value; the host validates the argument ranges, and
// the engine side queues the write for the target's own session (which owns
// the synchronisation).
using PlayerGamemodeSetter = std::function<int(const std::string& player, int gamemode)>;
using PlayerHealthSetter = std::function<int(const std::string& player, float health)>;
using PlayerFoodSetter = std::function<int(const std::string& player, int food, float saturation)>;

class Host {
public:
  // Defined in plugin_host.cpp. Public only so the C callback thunks (which
  // cannot be members of a C ABI table) can name it.
  struct Impl;

  Host();
  ~Host();
  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // Load every plugin under `dir`: each immediate subdirectory holding a
  // `+mc-plugin.toml`, or `dir` itself when it is one plugin. Manifests may
  // name dependencies (`depends = ["other"]`); plugins load and enable in an
  // order that puts every dependency first, and disable in reverse. A single
  // plugin that fails to load — or a dependency that is missing, refused or
  // part of a cycle — is logged, counted in `failed_count()` and skipped,
  // because one broken plugin must not keep a server from starting. A
  // directory that does not exist or cannot be read is still an error (false
  // + `*error`). Calling it twice replaces nothing: a second load adds to what
  // is there, and names already loaded satisfy later dependencies.
  bool load_directory(const std::string& dir, std::string* error);

  // Call every loaded plugin's on_enable once, in dependency order (load
  // order), with every peer already loaded. The server fires
  // PMC_EVENT_SERVER_START after this returns.
  void enable_all();

  // Disable and unload every plugin in reverse dependency order, so a
  // dependent is gone before the plugin it depends on. Idempotent.
  void unload_all();

  // One loaded plugin, as an operator surface reports it. `depends` is the
  // manifest's list, kept so a runtime unload can tell which plugins would be
  // left calling into a closed module.
  struct PluginInfo {
    std::string name;
    std::string version;
    bool enabled = false;
    std::vector<std::string> depends;
  };
  std::vector<PluginInfo> loaded_plugins() const;

  // Queue a plugin unload, or a hot reload (disable, unload, re-read the
  // manifest and config, open the module again, enable). The work happens at
  // the end of the next tick, never on the caller's thread: the caller may be
  // a session thread inside a handler, and a module must not be closed under
  // a callback that is still on the stack. A plugin another loaded plugin
  // depends on is refused, with `*error` naming the dependent. Returns true
  // when the request was accepted (the log carries the outcome), false when
  // it was refused and nothing was queued.
  bool request_reload(const std::string& name, std::string* error);
  bool request_unload(const std::string& name, std::string* error);

  std::size_t plugin_count() const;
  std::size_t failed_count() const;

  // The server's tick counter as plugins see it.
  uint64_t tick_count() const;

  // Advance the scheduler by `ticks` and deliver PMC_EVENT_TICK, then apply
  // any queued unload/reload requests. Called from the fixed-timestep loop;
  // safe with no plugins and no listeners.
  void tick(uint64_t ticks);

  // Deliver one event. Returns true when a cancelable event was cancelled.
  // `reply` may be empty; `player`/`message` may be null. `block` is only for
  // the block events and `item_use` only for PMC_EVENT_ITEM_USE_ON; each is
  // copied into the event's pointers for the call.
  bool fire(uint32_t type, const std::string* player, const std::string* message,
            const std::function<void(const std::string&)>& reply, bool cancelable,
            const BlockEvent* block = nullptr, const ItemUseEvent* item_use = nullptr,
            const EntityDeathEvent* death = nullptr);

  // Offer a tokenized command line to the plugins. Returns true when one
  // handled it. Built-in commands are not routed here (they keep precedence).
  bool dispatch_command(const std::string& player, const std::vector<std::string>& tokens,
                        const std::function<void(const std::string&)>& reply);

  // Wire the engine's player/world lookups into the ABI's accessors
  // (`pmc_host_api::player_get` / `world_block` / `world_biome`). main.cpp
  // registers these after the world exists and before plugins load; leaving
  // them unset makes every accessor answer "unavailable" rather than lie. The
  // callbacks run on whichever handler thread asks, so the engine side owns
  // its own synchronisation (the player registry and the world already do).
  void set_accessors(PlayerLookup player, BlockLookup block);

  // Wire the engine's authoritative block write into
  // `pmc_host_api::world_set_block`. Unset means every write answers
  // `PMC_BLOCK_NO_WORLD`, which is the truthful answer for a host with no engine
  // behind it. The callback runs on the plugin handler's thread and must return
  // an `enum pmc_block_result` value.
  void set_writer(BlockWriter block);

  // Wire the engine's entity registry into `pmc_host_api::entities`. Unset
  // answers -1, the same truthful "no world" a host with no engine behind it
  // gives the block accessors.
  void set_entities(EntityLookup entities);

  // Wire the engine's single-entity read into `pmc_host_api::entity_get`.
  // Unset answers `PMC_ENTITY_UNSUPPORTED`; the callback runs on the plugin
  // handler's thread and the engine side owns its own synchronisation.
  void set_entity_getter(EntityGetter getter);

  // Wire the engine's by-name entity lookup into `pmc_host_api::entity_find`.
  // Unset answers `PMC_ENTITY_UNSUPPORTED`; the callback runs on the plugin
  // handler's thread and owns its synchronisation like `entity_get`.
  void set_entity_finder(EntityFinder finder);

  // Wire the engine's entity writes into `pmc_host_api::entity_despawn`,
  // `entity_damage` and `entity_teleport`. Unset answers
  // `PMC_ENTITY_UNSUPPORTED`; the callbacks run on the plugin handler's thread
  // and own their synchronisation (the live registries already do).
  void set_entity_writes(EntityDespawner despawn, EntityDamager damage, EntityTeleporter teleport);

  // Wire the engine's player inventory into `pmc_host_api::inventory_get` and
  // `inventory_set`. Unset means reads answer -1 and writes
  // `PMC_INVENTORY_NO_PLAYER`; a host with no engine has no player to address.
  void set_inventory(InventoryReader read, InventoryWriter write);

  // Wire the engine's ops.json view into `command_level` and
  // `player_op_level`. Unset leaves every command at level 0 and answers -1; a
  // command whose level is above the caller's is refused before its handler.
  void set_op_levels(OpLevelLookup levels);

  // Wire the engine's world clock and weather into `world_time` /
  // `world_set_time` / `world_weather` / `world_set_weather`. Unset answers -1
  // for the reads and `PMC_WORLD_NO_WORLD` for the writes, the same truthful
  // "no world" the block accessors give a host with no engine behind it.
  void set_world_state(WorldTime time, WorldTimeSetter set_time, WeatherReader weather,
                       WeatherSetter set_weather);

  // Wire the engine's chat delivery into `player_message`, `broadcast` and
  // `player_count`. Unset answers `PMC_MESSAGE_NO_PLAYER` for a direct message,
  // 0 for a broadcast and -1 for the count — the truthful answers for a host
  // with no players behind it.
  void set_messaging(PlayerMessenger message, Broadcaster broadcast, PlayerCounter count);

  // Wire the engine's player-state writes into `player_set_gamemode`,
  // `player_set_health` and `player_set_food`. Unset answers
  // `PMC_PLAYER_UNSUPPORTED`; the callbacks run on the plugin handler's thread
  // and queue the write for the target's session, so Ok means accepted.
  void set_player_state(PlayerGamemodeSetter gamemode, PlayerHealthSetter health,
                        PlayerFoodSetter food);

private:
  std::unique_ptr<Impl> impl_;
};

// The process-wide host the server wires into the tick loop and the session
// path. Constructed on first use; empty until load_directory is called.
Host& host();

// Session-side helpers (what src/server/session.cpp calls).
bool fire_player_chat(const std::string& player, const std::string& message,
                      const std::function<void(const std::string&)>& reply);

// Fire the player lifecycle events from the session that owns the player. Join
// carries the joining player's reply handle; leave carries only the last known
// name (the session is gone, so there is nobody to answer). Neither is
// cancelable: the player is already in or out of the world.
void fire_player_join(const std::string& player,
                      const std::function<void(const std::string&)>& reply);
void fire_player_leave(const std::string& player);

// Fire a player's block interaction from the session that owns them. Returns
// true when a plugin cancelled it; the caller then must not write the world
// (and should re-send the cell's existing state to the predicting client).
bool fire_block_place(const std::string& player, const BlockEvent& block,
                      const std::function<void(const std::string&)>& reply);
bool fire_block_break(const std::string& player, const BlockEvent& block,
                      const std::function<void(const std::string&)>& reply);

// Fire a bucket or flint-and-steel use on a block. Returns true when a plugin
// cancelled it; the caller must not apply the item's effect (no fluid, TNT,
// fire, wear or stat) and should restore the client's prediction.
bool fire_item_use_on(const std::string& player, const ItemUseEvent& use,
                      const std::function<void(const std::string&)>& reply);

// Fire a mob death from the thread that resolved it (the world tick, or the
// killer's session). Not cancelable; the loot and kill credit are already
// resolved when this runs.
void fire_entity_death(const EntityDeathEvent& death);
bool dispatch_command(const std::string& player, const std::vector<std::string>& tokens,
                      const std::function<void(const std::string&)>& reply);

} // namespace mc::plugins
