// +MC native plugin ABI, version 1.
//
// This header is the *only* thing a plugin has to compile against. It is a C
// ABI on purpose: a .so/.dylib built once keeps loading across engine builds
// that share ABI 1, and the compiler/language a plugin is written in does not
// have to match the server's.
//
// Stability: `PMC_PLUGIN_ABI_VERSION` (and the `api` key in the plugin's
// manifest) names this layout. Adding a field to the end of a struct is a
// compatible change when the struct starts with `struct_size`; removing or
// re-ordering one is not, and moves the version. The host reads at most
// `struct_size` bytes of a descriptor and hands the plugin a host table whose
// own `struct_size` a newer host may grow.
//
// Threading contract (the part a plugin cannot discover by itself):
//
//   * `on_load` and `on_enable` run once, on the thread that started the
//     server, before the first event is delivered.
//   * `PMC_EVENT_TICK` and every scheduler callback run on the server's tick
//     loop thread.
//   * `PMC_EVENT_PLAYER_CHAT`, `PMC_EVENT_PLAYER_JOIN`,
//     `PMC_EVENT_PLAYER_LEAVE`, `PMC_EVENT_BLOCK_PLACE`,
//     `PMC_EVENT_BLOCK_BREAK`, `PMC_EVENT_ITEM_USE_ON` and every command
//     callback run on the *session* thread that owns the player's connection.
//     Several players mean several threads: handlers for different players
//     may run concurrently.
//   * `on_disable` and `on_unload` run on the main thread after the tick loop
//     has stopped.
//
// A handler must be quick and must not block (a session thread is also the
// connection's reader). The host serialises its own registries, so the host
// API may be called from any of those threads; a plugin's own state needs its
// own locking where two session threads can reach it. An exception escaping a
// C++ callback is caught by the host, logged, and the plugin stays loaded —
// but it is a bug, not a mechanism.
#ifndef PLUSMC_PLUGIN_H
#define PLUSMC_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bumped when this layout changes incompatibly. A plugin built against a
// different value is refused at load with a line naming both numbers.
#define PMC_PLUGIN_ABI_VERSION 1u

// The manifest key `api` has to match the host's ABI version.
#define PMC_PLUGIN_MANIFEST_API 1

#if defined(_WIN32)
#define PMC_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define PMC_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define PMC_PLUGIN_EXPORT
#endif

enum pmc_log_level { PMC_LOG_DEBUG = 0, PMC_LOG_INFO = 1, PMC_LOG_WARN = 2, PMC_LOG_ERROR = 3 };

enum pmc_event_type {
  // After every plugin is loaded and enabled, once per process.
  PMC_EVENT_SERVER_START = 1,
  // Before any plugin is disabled or unloaded, once per process.
  PMC_EVENT_SERVER_STOP = 2,
  // Once per scheduler pump (the tick loop), with `tick` set to the server's
  // tick count. Cheap with no listeners: the host does not build an event.
  PMC_EVENT_TICK = 3,
  // A chat line from a player, before it is relayed to anyone. `player` is the
  // sender's name, `message` the raw text and `reply` a handle that answers the
  // sender. Cancelable: setting `cancel` drops the broadcast (and the log line)
  // for everyone, which is how a plugin implements muted players or a chat
  // filter. A slash command never reaches this event: it is a command.
  PMC_EVENT_PLAYER_CHAT = 10,
  // A player is in play: the join burst is done, the registry entry is live
  // and the player can receive a reply. `player` is the name, `message` is null
  // and `reply` answers the joining player. Not cancelable — the player is
  // already in the world.
  PMC_EVENT_PLAYER_JOIN = 11,
  // A player has left: the registry entry is gone and the leave burst has been
  // sent, so an accessor can no longer find them. `player` is the last known
  // name, `message` and `reply` are null. Not cancelable. Fires only for a
  // join the host announced.
  PMC_EVENT_PLAYER_LEAVE = 12,
  // A player is about to place a block. `player` is the placer, `reply` answers
  // them, and the block fields describe the cell: `block` is the canonical
  // state that would be placed, `previous` the state the cell holds now.
  // Cancelable: setting `cancel` refuses the placement — the world is not
  // written, the item is not consumed, the client is sent the cell's current
  // state and the queued slot, and the handler may tell the player why through
  // `reply`. Fired on the session thread, before the world write.
  PMC_EVENT_BLOCK_PLACE = 20,
  // A player is about to break a block. `player` is the breaker, `reply`
  // answers them, `block` and `previous` are both the state being removed.
  // Cancelable: setting `cancel` keeps the block (the client gets the state
  // back) and skips the drops, stats, container spill and durability wear.
  // Fired before any of those side effects, on the session thread. Only a
  // completed player break reaches it; explosions, pistons and other
  // engine-driven removals do not.
  PMC_EVENT_BLOCK_BREAK = 21,
  // A player is about to use an item on a block. Today that is a bucket
  // (empty or filled) or flint and steel, which includes priming a TNT block;
  // a block placement is `PMC_EVENT_BLOCK_PLACE` instead. `player` is the user,
  // `reply` answers them, `item` is the held item's canonical name, and the
  // block fields describe the clicked cell: `block` is its canonical state,
  // `block_id` its engine state id (-1 when it has none), and `face` the
  // clicked face (0 down, 1 up, 2 north, 3 south, 4 west, 5 east). `previous`
  // is null. Cancelable: setting `cancel` skips the item's effect (no fluid
  // moved, no TNT primed, no fire lit, no wear or stat), restores the clicked
  // and target cells the client may have predicted, resends the used slot and
  // acknowledges the sequence. Fired on the session thread, before the item's
  // effect is attempted.
  PMC_EVENT_ITEM_USE_ON = 30,
  // A mob died. Fires once per death, after the kill credit, loot and
  // experience are resolved, on the thread that resolved it: the world tick for
  // a mob that fell, burned or was killed by another mob, the killer's session
  // thread for a player kill. Not cancelable — the death already happened.
  // `entity_id` is the dead mob's id, `entity_type` its registry name (e.g.
  // `minecraft:cow`), and `entity_x`/`entity_y`/`entity_z` the position the
  // loot was dropped at. `killer_entity_id` and `killer_name` name the credited
  // killer, or -1/null when nothing was credited. A player death is not this
  // event; it is the session's own death message path.
  PMC_EVENT_ENTITY_DEATH = 40
};

typedef struct pmc_host pmc_host;
// Opaque handle for answering the player (or the console) an event or command
// came from. Pass it back to `pmc_host_api::reply`.
typedef struct pmc_reply pmc_reply;

typedef struct pmc_event {
  // sizeof(pmc_event) as the plugin's header saw it.
  uint32_t struct_size;
  uint32_t type;      // enum pmc_event_type
  int cancelable;     // 1 when `cancel` is honoured for this type
  int cancel;         // handler sets this to 1 to cancel
  const char* player; // may be NULL (server events, scheduler)
  const char* message;
  uint64_t tick;    // PMC_EVENT_TICK: the server's tick count
  pmc_reply* reply; // may be NULL; valid only during the handler

  // ---- block and item-use events (appended; zero/null otherwise) -----------
  // The cell a PMC_EVENT_BLOCK_PLACE / PMC_EVENT_BLOCK_BREAK / PMC_EVENT_ITEM_USE_ON
  // is about, the canonical state name of the block
  // being placed or broken (for item use, the clicked block's state), the
  // state the cell held before a block action, and that block's engine state
  // id (-1 when the name has no id). `block` and `previous` are host-owned
  // strings valid only during the handler; copy them if a plugin needs them
  // afterwards. `previous` is null for item use.
  int32_t block_x, block_y, block_z;
  const char* block;
  const char* previous;
  int32_t block_id;

  // ---- PMC_EVENT_ITEM_USE_ON (appended; zero/null otherwise) ---------------
  // The canonical name of the item in the hand that was used, and the clicked
  // face (0 down, 1 up, 2 north, 3 south, 4 west, 5 east). `item` is a
  // host-owned string valid only during the handler.
  const char* item;
  int32_t face;

  // ---- PMC_EVENT_ENTITY_DEATH (appended; zero/null otherwise) -------------
  // The dead mob's entity id and registry type name, the position its loot was
  // dropped at, and the credited killer (-1 / null when nothing was credited).
  // `entity_type` and `killer_name` are host-owned strings valid only during
  // the handler.
  int32_t entity_id;
  const char* entity_type;
  double entity_x, entity_y, entity_z;
  int32_t killer_entity_id;
  const char* killer_name;
} pmc_event;

typedef struct pmc_command {
  uint32_t struct_size;
  const char* name;        // lower-case, no leading '/'
  int argc;                // number of tokens, argv[0] is the name
  const char* const* argv; // valid only during the handler
  const char* player;      // who typed it
  pmc_reply* reply;
} pmc_command;

typedef void (*pmc_event_fn)(void* plugin_data, pmc_event* event);
typedef void (*pmc_command_fn)(void* plugin_data, pmc_command* command);
typedef void (*pmc_task_fn)(void* plugin_data);

// A player currently in play, as a *snapshot*: the fields are copied under the
// host's own lock, so a player who leaves mid-read cannot dangle a pointer at
// the plugin. Set `struct_size` to sizeof(pmc_player) before the call; the host
// fills at most that many bytes and at most its own sizeof(pmc_player), then
// sets `struct_size` to the number it filled. A caller built against a newer
// header therefore sees a smaller value from an older host and must not read
// the bytes past it.
typedef struct pmc_player {
  uint32_t struct_size;
  char name[64]; // NUL-terminated, the server's spelling
  double x, y, z;
  float yaw, pitch; // degrees
  int gamemode;     // 0 survival, 1 creative, 2 adventure, 3 spectator
  float health;
  int32_t entity_id;
  // Delta movement in blocks per tick, the same observed stance delta
  // `pmc_entity` reports: the distance between the last two position packets
  // divided by the ticks between them, 0 until the second packet and after a
  // teleport. Appended after the original fields, so an older plugin's struct
  // ends at `entity_id` and the host leaves the missing tail alone.
  double vx, vy, vz;
} pmc_player;

// One live entity, as a *snapshot* handed to an `entities` visitor. Nothing
// here is live: the id, type, position, health, motion and facing are copied
// under the owning registry's lock before the callback runs, so an entity that
// dies or walks during the visit cannot dangle a handle at the plugin. Strings
// are NUL-terminated and truncated to fit.
//
typedef struct pmc_entity {
  // The size of the snapshot the host filled: `entities` reports its own
  // sizeof(pmc_entity) for the host-owned snapshot, and `entity_get` reports
  // how many bytes it copied — smaller than the caller's declared size when
  // the caller's header is newer than the host's. A caller built against a
  // newer header must check this before reading fields the host may not have.
  uint32_t struct_size;
  int32_t entity_id;
  // The minecraft:entity_type protocol id (`Add Entity`'s type varint).
  int32_t type_id;
  char type[64]; // registry name, e.g. "minecraft:zombie"
  // The entity's display name: a mob's type name ("Zombie"), a player's name,
  // an item entity's item name ("minecraft:stone"), and the type name for the
  // rest.
  char name[64];
  double x, y, z; // feet position, the same frame `pmc_player` uses
  float health;   // 0 for an entity with no health pool
  int32_t age;    // Entity.tickCount (0 where the engine does not track one)
  // Delta movement in blocks per tick (Entity.getDeltaMovement). A player's is
  // observed, not simulated: the stance delta between its last two position
  // packets divided by the ticks between them, 0 until the second packet and
  // after a teleport. An area-effect cloud has no motion of its own.
  double vx, vy, vz;
  // Look angles in degrees, the same convention `pmc_player` uses (yaw 0 faces
  // +z, pitch positive down). 0 where the kind does not track rotation (drops,
  // orbs, primed TNT, clouds). A mob carries yaw only, so its pitch is 0; an
  // arrow or thrown potion reports the rotation its velocity implies, which is
  // the same one its spawn packet uses.
  float yaw, pitch;
} pmc_entity;

// Visits one entity. The pointer is valid only for the duration of the call;
// copy what the plugin needs. An exception escaping it is caught by the host.
typedef void (*pmc_entity_fn)(void* plugin_data, const pmc_entity* entity);

// One player-inventory stack, as a snapshot (`pmc_host_api::inventory_get`).
// `item` is the item registry id; 0 (minecraft:air) and count 0 mean empty.
// Components are not exposed by ABI 1: `damage` is the durability the stack has
// used, and enchantments/potion contents/trims are read-only engine state.
typedef struct pmc_item {
  uint32_t struct_size;
  int32_t item;
  int32_t count;
  int32_t damage;
  int32_t max_stack_size; // 0 when the slot is empty
  char name[64];          // canonical name, "" when empty
} pmc_item;

// The result of an inventory accessor. The values are stable ABI: a plugin may
// compare against them or against 0 for success.
enum pmc_inventory_result {
  PMC_INVENTORY_OK = 0,
  // No player of that name is in play.
  PMC_INVENTORY_NO_PLAYER = -1,
  // The slot is outside the player inventory (0..40), `count` is negative or
  // larger than the item's maximum stack size, or a `pmc_item`'s `struct_size`
  // is smaller than this header's `sizeof(pmc_item)`.
  PMC_INVENTORY_OUT_OF_RANGE = -2,
  // The item name is not one the registry knows.
  PMC_INVENTORY_UNKNOWN_ITEM = -3,
  // The write could not be queued for the player's session.
  PMC_INVENTORY_WRITE_FAILED = -4
};

// The result of an entity write (`pmc_host_api::entity_despawn`,
// `entity_damage`, `entity_teleport`). The values are stable ABI.
enum pmc_entity_result {
  PMC_ENTITY_OK = 0,
  // No live entity of that id is in the world (it died, was discarded, or the
  // id was never handed out).
  PMC_ENTITY_NOT_FOUND = -1,
  // The write does not apply to that entity: a player cannot be despawned, and
  // damage only lands on a living entity (a mob or a player), not on an item,
  // orb, projectile, TNT or cloud.
  PMC_ENTITY_UNSUPPORTED = -2,
  // The arguments cannot be used: a non-positive or non-finite damage amount,
  // or a non-finite teleport coordinate.
  PMC_ENTITY_INVALID = -3,
  // The write was well formed but the entity refused it — a hit inside the
  // invulnerability window, or a player session that left before its queued
  // write could be applied.
  PMC_ENTITY_REFUSED = -4
};

// The result of a block write (`pmc_host_api::world_set_block`). The values are
// stable ABI: a plugin may compare against them or against 0 for success.
enum pmc_block_result {
  PMC_BLOCK_OK = 0,
  // The server has no world registered (the void mode before one is served).
  PMC_BLOCK_NO_WORLD = -1,
  // The position is outside the world's build limits.
  PMC_BLOCK_OUT_OF_RANGE = -2,
  // The state name is not a block state the registry knows, or names a
  // property the block does not carry.
  PMC_BLOCK_UNKNOWN_STATE = -3,
  // The world refused the write (a column that could not be made resident).
  PMC_BLOCK_WRITE_FAILED = -4
};

// The level's weather: what `world_weather` reports and `world_set_weather`
// takes. Thunder also rains, in vanilla and here.
enum pmc_weather { PMC_WEATHER_CLEAR = 0, PMC_WEATHER_RAIN = 1, PMC_WEATHER_THUNDER = 2 };

// The result of a world-state write (`world_set_time`, `world_set_weather`).
// The values are stable ABI.
enum pmc_world_result {
  PMC_WORLD_OK = 0,
  // The server has no level (or world clock) behind it.
  PMC_WORLD_NO_WORLD = -1,
  // The argument cannot be used: negative ticks, or a mode outside `pmc_weather`.
  PMC_WORLD_INVALID = -2
};

// The result of a player message (`pmc_host_api::player_message`). The values
// are stable ABI.
enum pmc_message_result {
  PMC_MESSAGE_OK = 0,
  // No player of that name is in play.
  PMC_MESSAGE_NO_PLAYER = -1,
  // The line is null, empty or longer than the ABI's 4096-byte limit.
  PMC_MESSAGE_INVALID = -2
};

// The result of a player-state write (`player_set_gamemode`,
// `player_set_health`, `player_set_food`). The values are stable ABI.
enum pmc_player_result {
  PMC_PLAYER_OK = 0,
  // No player of that name is in play.
  PMC_PLAYER_NO_PLAYER = -1,
  // The host has no engine behind it, so there is no player to write.
  PMC_PLAYER_UNSUPPORTED = -2,
  // The argument cannot be used: a game mode outside 0..3, a health value
  // outside (0, 20], a food level outside 0..20, or a non-finite saturation.
  PMC_PLAYER_INVALID = -3,
  // Reserved for an engine-side refusal of a well-formed write; the current
  // engine answers `PMC_PLAYER_NO_PLAYER` when the target disappears before
  // the queue.
  PMC_PLAYER_REFUSED = -4
};

// The result of a config write (`config_set_string`, `config_set_int`,
// `config_set_bool`). The values are stable ABI.
enum pmc_config_result {
  PMC_CONFIG_OK = 0,
  // The key is empty, longer than 64 bytes, or carries a character outside
  // [A-Za-z0-9_.-]; a string is null or contains a quote or newline; or a
  // boolean is not 0/1.
  PMC_CONFIG_INVALID = -1,
  // The new `config.toml` could not be written. The old value and the old file
  // are unchanged.
  PMC_CONFIG_IO = -2
};

// The host table a plugin is given in `on_load`. Every function is safe to
// call from the plugin's handlers.
typedef struct pmc_host_api {
  // sizeof(pmc_host_api) as the *host* built it. A plugin only reads fields
  // it knows about, so a host with a larger table stays compatible.
  uint32_t struct_size;
  uint32_t abi_version;
  void* host; // opaque, pass back as the first argument

  // Write a line to the server log. Prefixed with the plugin's name.
  void (*log)(void* host, int level, const char* message);

  // Subscribe to an event type. Allowed while on_load/on_enable runs; returns
  // 0 on success, -1 on an unknown type or when the host is not inside a
  // lifecycle callback. Unsubscribing is by unloading the plugin. Every
  // listener for an event runs, even when an earlier one cancels: the host
  // honours a cancel from any of them.
  int (*on)(void* host, uint32_t event_type, pmc_event_fn fn, void* plugin_data);

  // Register a command by its bare name (`greet`, not `/greet`). Built-in
  // commands keep precedence: a plugin is only consulted for a line no
  // built-in accepted. Returns 0 on success, -1 if the name is already taken.
  int (*command)(void* host, const char* name, const char* usage, pmc_command_fn fn,
                 void* plugin_data);

  // Answer the player an event or command came from. `reply` may be NULL (a
  // tick or scheduler callback has no player), in which case nothing is sent;
  // `message` must be NUL-terminated and is copied by the host.
  void (*reply)(void* host, pmc_reply* reply, const char* message);

  // Run `fn` on the tick loop after `delay_ticks` server ticks, and every
  // `repeat_ticks` after that when it is non-zero. A delay of 0 means "the
  // next pump". Returns a non-zero id usable with `cancel`, or 0 on failure.
  uint64_t (*schedule)(void* host, uint32_t delay_ticks, uint32_t repeat_ticks, pmc_task_fn fn,
                       void* plugin_data);
  int (*cancel)(void* host, uint64_t task_id);

  // The current server tick count (monotonic, starts near 0).
  uint64_t (*tick_count)(void* host);

  // The engine version string, e.g. "plusmc 0.6.0 (protocol 777)".
  const char* (*server_version)(void* host);

  // ---- world & player accessors (ABI 1, appended) -------------------------
  //
  // A snapshot of a player currently in play, looked up by name
  // (case-insensitive). `out->struct_size` must cover the original
  // `pmc_player` (through `entity_id`, trailing padding included) — a plugin
  // built against the header before velocity was appended. A larger caller
  // sets its own size and receives every field this host knows; on return
  // `struct_size` is the number of bytes filled, so an older host is visible
  // as a smaller value.
  // Returns 0 on success, -1 when no such player is connected or the struct
  // is too small. Safe from any handler thread; the answer is a copy, not a
  // live handle.
  int (*player_get)(void* host, const char* name, pmc_player* out);

  // The canonical state name at a world position, e.g.
  // "minecraft:oak_stairs[facing=east,half=bottom]" (properties in the disk's
  // sorted form). `out` is NUL-terminated and truncated to `capacity` bytes.
  // Returns 0 on success, -1 when the server has no world, -2 when `out` is
  // null or `capacity` is too small for the whole name. The read goes through
  // the world's own load path, like vanilla's `Level.getBlockState`: a column
  // that is not resident may be read from disk or generated, so a position
  // read belongs in a command or an event, not in a per-tick loop over many
  // positions.
  int (*world_block)(void* host, int32_t x, int32_t y, int32_t z, char* out, uint32_t capacity);

  // The biome's registry name at a world position (`minecraft:plains`). Same
  // return values as `world_block`.
  int (*world_biome)(void* host, int32_t x, int32_t y, int32_t z, char* out, uint32_t capacity);

  // ---- world mutation (ABI 1, appended) -----------------------------------
  // Set the block at a world position through the engine's own block-change
  // path, the one `/setblock` uses in its default replace mode: the world
  // state, every client that holds the chunk, the neighbour fan-out (leaves and
  // fluids), the redstone graph and a new fire's scheduled clock all see the
  // write. A plugin never pokes the column behind the engine's back. (The
  // command's `destroy` drop and `keep` gate are policy, not the write path; a
  // plugin that wants either reads the old block with `world_block` first.)
  //
  // `state` is a canonical block-state name in the form `world_block` returns,
  // e.g. "minecraft:oak_stairs[facing=east,half=bottom]". A name without a
  // namespace is taken as `minecraft:`, and omitted properties keep the
  // block's default values. An unknown block or property is rejected rather
  // than silently ignored. Returns an `enum pmc_block_result`; on success the
  // write is already visible to a `world_block` call and queued to clients.
  //
  // The call is synchronous and runs on the calling handler's thread. It is
  // safe from any handler thread; the engine serialises the world write
  // itself. A plugin that writes many blocks should do so from a command or a
  // scheduled task, not from a per-tick loop.
  int (*world_set_block)(void* host, int32_t x, int32_t y, int32_t z, const char* state);

  // ---- entity listing (ABI 1, appended) -----------------------------------
  // Visit every entity currently in the world, once each: players, mobs,
  // dropped items, experience orbs, primed TNT, arrows and thrown items, thrown
  // potions and their area-effect clouds. The list is a snapshot assembled
  // under each registry's own lock, so it is coherent per registry rather than
  // across all of them — an entity that moves between two registries' reads is
  // reported at its position when that registry was read. The visit runs on the
  // calling thread; the host holds no lock while the callback runs, so the
  // plugin may call any host function from it (including a write). Returns the
  // number of entities visited, or -1 when the server has no world registered.
  int (*entities)(void* host, pmc_entity_fn fn, void* plugin_data);

  // ---- player inventory (ABI 1, appended) ---------------------------------
  // A copy of one slot of a player's inventory. Slots are the player inventory's
  // own numbering: 0..8 hotbar, 9..35 main, 36..39 armour (feet, legs, chest,
  // head), 40 offhand. Note this is *not* the menu numbering the wire uses
  // (inventory.h documents both). `out->struct_size` must be at least
  // `sizeof(pmc_item)` or the call answers `PMC_INVENTORY_OUT_OF_RANGE`; every
  // field of the struct is filled, and an empty slot
  // answers `PMC_INVENTORY_OK` with `item`/`count` zero. The answer is the
  // snapshot the player's session published on its last processed packet, the
  // same freshness `player_get`'s position has, because the live inventory
  // belongs to that session's thread.
  int (*inventory_get)(void* host, const char* player, int32_t slot, pmc_item* out);

  // Queue a write to one slot of a player's inventory, by item name (the same
  // `minecraft:`-optional form `world_set_block` takes). `count` 0 clears the
  // slot; otherwise `count` is 1..the item's maximum stack size. The write is
  // applied by the target player's own session on its next tick, through the
  // same path a menu click takes: the stack moves, the menu revision advances
  // and the client is sent the slot. When the open menu does not show the slot
  // (armour or the offhand under a crafting table), the open menu is resynced
  // and the equipment relay carries an armour or hand change to the other
  // players. A plugin therefore sees
  // `PMC_INVENTORY_OK` for a *queued* write, not a completed one; the player's
  // next `inventory_get` reflects it. Returns an `enum pmc_inventory_result`.
  int (*inventory_set)(void* host, const char* player, int32_t slot, const char* item,
                       int32_t count);

  // ---- entity writes (ABI 1, appended) ------------------------------------
  // Discard one entity by id: `Entity.discard` rather than a death, so there is
  // no death event, no loot and no experience — the entity leaves the world's
  // registry immediately, and every client tracking it is sent Remove Entities
  // in the same call. The tracking relationship ends with it. Players cannot be
  // despawned (`PMC_ENTITY_UNSUPPORTED`); every other entity the `entities`
  // listing reports can. Returns an `enum pmc_entity_result`.
  int (*entity_despawn)(void* host, int32_t entity_id);

  // Damage one living entity by id, through the engine's own hurt path: a mob's
  // hit lands synchronously under the registry lock and its viewers are sent the
  // damage event, the hurt animation and its new health; a lethal hit leaves the
  // death to the world tick, exactly as a sword hit does, so the death event,
  // the drops and the experience still arrive from the tick that runs the kill.
  // A player's health belongs to their own session thread, so that half is
  // *queued* (the same queue `/damage <player> <amount>` uses) and the call
  // reports `PMC_ENTITY_OK` for an accepted hit, not a completed one. `amount`
  // must be positive and finite. Returns an `enum pmc_entity_result`.
  int (*entity_damage)(void* host, int32_t entity_id, float amount);

  // Move one entity by id to a position, zeroing its velocity: the engine's
  // teleport path, not a raw field poke. The position is written under the
  // owning registry's lock and every client tracking the entity is sent the
  // authoritative position sync in the same call (tracking is re-evaluated on
  // the next tick's sweep, which pairs or drops viewers the move crossed). A
  // player is moved by their own session on its next tick, through the same
  // queued action `/tp <player> <x> <y> <z>` uses; the call reports
  // `PMC_ENTITY_OK` for an accepted move, not a completed one. Returns an
  // `enum pmc_entity_result`.
  int (*entity_teleport)(void* host, int32_t entity_id, double x, double y, double z);

  // ---- entity lookup (ABI 1, appended) ------------------------------------
  // A snapshot of one live entity by id, the single-entity counterpart of
  // `entities`: the same `pmc_entity` fields, taken under the owning registry's
  // lock, so the answer cannot dangle. `out->struct_size` must be at least the
  // original fields through `age`; a caller built against an older header sets
  // its smaller struct and receives that prefix, while a caller built against
  // this header sets `sizeof(pmc_entity)` and also receives velocity and
  // facing. The host copies at most the bytes the caller declares and at most
  // its own sizeof(pmc_entity), then sets `struct_size` to the number filled,
  // so a newer caller can tell that an older host stopped short. Returns
  // `PMC_ENTITY_OK`, `PMC_ENTITY_NOT_FOUND` when no
  // live entity has that id (it died, was discarded, or never existed),
  // `PMC_ENTITY_INVALID` for a non-positive id or a struct that is too small,
  // and `PMC_ENTITY_UNSUPPORTED` when the host has no engine behind it. The
  // lookup is one registry read, not a walk of the whole `entities` listing.
  int (*entity_get)(void* host, int32_t entity_id, pmc_entity* out);

  // ---- plugin environment (ABI 1, appended) -------------------------------
  // The absolute path of a directory the host owns for this plugin's files:
  // `<plugin directory>/data`, created before `on_load` runs. The string is
  // owned by the host and stays valid until the plugin is unloaded; a plugin
  // must copy it if it needs the path later. Returns NULL outside a plugin
  // callback. A plugin that wants a config file keeps it here rather than in
  // its manifest directory, so reinstalling the module does not overwrite it.
  const char* (*data_dir)(void* host);

  // ---- plugin config (ABI 1, appended) ------------------------------------
  // Values from `<data_dir>/config.toml`, a flat `key = value` file the host
  // reads before `on_load`; the appended writers below change the live values
  // and rewrite that file. Keys are case-sensitive. `config_string` answers a
  // quoted string, or NULL when the key is absent or is not a string; the
  // pointer is owned by the host and stays valid until unload, including
  // across a later write of the same key.
  const char* (*config_string)(void* host, const char* key);
  // An integer value, or `fallback` when the key is absent or has another type.
  int64_t (*config_int)(void* host, const char* key, int64_t fallback);
  // A `true`/`false` value as 1/0, or `fallback` when absent or another type.
  int (*config_bool)(void* host, const char* key, int fallback);

  // ---- async work (ABI 1, appended) ---------------------------------------
  // Queue `fn` to run once on the host's worker thread, off the tick and
  // session threads. Intended for blocking work (file or database I/O) a
  // plugin must not do on a tick. The callback may call any host function, but
  // there is no player reply handle and it must not touch plugin state that a
  // session handler touches without its own lock. Returns a non-zero id usable
  // with `cancel`, or 0 when the host is shutting a plugin set down. Cancelling
  // a queued task prevents it from running; a task already running cannot be
  // interrupted. Unload waits for queued and running async work, so a plugin is
  // never closed underneath its own callback.
  uint64_t (*schedule_async)(void* host, pmc_task_fn fn, void* data);

  // ---- command permissions (ABI 1, appended) ------------------------------
  // Register a command that requires at least `op_level` (0..4). Level 0 is
  // every player; 1..4 follow the server's operator levels. A server with a
  // non-empty ops.json already restricts commands to operators, so these
  // levels refine which operators may run a plugin command. Returns 0 on
  // success, -1 for an invalid level or an already-taken name.
  int (*command_level)(void* host, const char* name, const char* usage, int op_level,
                       pmc_command_fn fn, void* data);
  // The player's operator level (0..4), or -1 when the name is not a listed
  // operator or the host has no ops lookup. A read: it does not change access.
  int (*player_op_level)(void* host, const char* name);

  // ---- world clock and weather (ABI 1, appended) --------------------------
  // The overworld's game time in ticks (0 = dawn on day 0, 24000 ticks a day),
  // the same clock `advance_world_time` moves and SetTime carries. Readable on
  // any handler thread; the clock is atomic. Returns -1 when the host has no
  // world clock behind it.
  int64_t (*world_time)(void* host);

  // Set the overworld clock to `ticks` and send every player the SetTime update
  // in the same call, exactly as `/time set` does. `ticks` is the total game
  // time and must be >= 0. Returns an `enum pmc_world_result`.
  int (*world_set_time)(void* host, int64_t ticks);

  // The level's weather right now: one of `enum pmc_weather`, or
  // `PMC_WORLD_NO_WORLD` when the server has no level behind it. This is the
  // mode that was set, not the ramped rain level a client is drawing.
  int (*world_weather)(void* host);

  // Set the level's weather, like `/weather <clear|rain|thunder> [duration]`.
  // `duration_ticks` <= 0 samples the mode's own default (vanilla's clear, rain
  // and thunder distributions); a positive value is that many ticks. The level
  // ramps its rain/thunder levels over the following ticks and every player
  // receives the same game events the command broadcasts. Returns an
  // `enum pmc_world_result`.
  int (*world_set_weather)(void* host, int weather, int32_t duration_ticks);

  // ---- player messaging (ABI 1, appended) ---------------------------------
  // Send one chat line to the player with that name (case-insensitive): the
  // same `SystemChat` (0x79) the server's own chat and command feedback use,
  // built by the same code. `message` is the whole line and is rendered as one
  // plain text component, so there is no chat `<name>` decoration, translation
  // key, hover or click event unless the text itself carries it (and `<` stays
  // literal, because the component writer escapes it). 1..4096 UTF-8 bytes; a
  // null, empty or longer line answers `PMC_MESSAGE_INVALID` rather than being
  // silently truncated. Safe from any handler thread: the registry is
  // serialised, and a player found under its lock cannot disconnect before the
  // write. Returns an `enum pmc_message_result`.
  int (*player_message)(void* host, const char* player, const char* message);

  // The same line to every player in play. Returns the number of players
  // reached (0 when nobody is connected), or `PMC_MESSAGE_INVALID`.
  int (*broadcast)(void* host, const char* message);

  // How many players are in play right now, or -1 when the host has no engine
  // behind it.
  int (*player_count)(void* host);

  // ---- player state writes (ABI 1, appended) ------------------------------
  // Set a player's game mode (0 survival, 1 creative, 2 adventure, 3
  // spectator) through the engine's own change path: the registry entry, the
  // client's GameEvent + Abilities + PlayerInfoUpdate and the tab-list
  // broadcast all move together, exactly as `/gamemode <mode> <player>` does.
  // The write is queued for the target's own session and applied on its next
  // tick, so `PMC_PLAYER_OK` means accepted, not completed; a player who
  // disconnects in between never applies it. Returns `enum pmc_player_result`.
  int (*player_set_gamemode)(void* host, const char* player, int gamemode);

  // Set a player's health in half-hearts, 0 < `health` <= 20 (the base
  // maximum; attribute modifiers such as Health Boost are not offered here).
  // The write is queued for the target's session, applied to the same survival
  // state the engine heals and damages, and the client's `set_health` (and
  // every other client's health word) follows in the call that applies it. A
  // dead player is not revived: a queued health write for a corpse is consumed
  // without effect. Returns `enum pmc_player_result`.
  int (*player_set_health)(void* host, const char* player, float health);

  // Set a player's food level (0..20) and saturation. Saturation is clamped to
  // the food level, the same `FoodData.setSaturation` clamp vanilla applies.
  // Queued like the other two writes; the client's `set_health` carries both
  // numbers. Returns `enum pmc_player_result`.
  int (*player_set_food)(void* host, const char* player, int food, float saturation);

  // ---- entity name lookup (ABI 1, appended) -------------------------------
  // Find the first live entity whose name matches `name` case-insensitively
  // and copy its snapshot, the by-name counterpart of `entity_get`. The order
  // is fixed: players by account name, then mobs by their type's display name
  // ("Zombie") or registry name ("minecraft:zombie"), then dropped items by
  // item name, then experience orbs, primed TNT, projectiles, thrown potions
  // and area-effect clouds by their display or type name. The first match wins;
  // a name shared by several entities resolves to the first in registry order,
  // so `entities` remains the way to walk them all. `out` follows the same
  // `struct_size` rule as `entity_get`: at least the original fields through
  // `age`, and on return the number of bytes filled. Returns `PMC_ENTITY_OK`,
  // `PMC_ENTITY_NOT_FOUND`, `PMC_ENTITY_INVALID` for a null/empty name or a
  // too-small struct, and `PMC_ENTITY_UNSUPPORTED` when the host has no engine
  // behind it.
  int (*entity_find)(void* host, const char* name, pmc_entity* out);

  // ---- plugin config writes (ABI 1, appended) -----------------------------
  // Set one key and persist the whole flat config to `<data_dir>/config.toml`.
  // Keys are 1..64 bytes of [A-Za-z0-9_.-]; a string must not contain a quote
  // or a newline, and a boolean is 0/1. Writing a key that already exists
  // changes its type. The live value is visible to the getters immediately;
  // the file is replaced atomically after a successful write, so a refused
  // call changes neither memory nor disk. Only a plugin's own callback may
  // write (the owner is the running plugin), and writes are serialized per
  // plugin. The rewritten file keeps every value, sorted by key; comments and
  // the operator's formatting are not preserved. Returns `enum
  // pmc_config_result`.
  int (*config_set_string)(void* host, const char* key, const char* value);
  int (*config_set_int)(void* host, const char* key, int64_t value);
  int (*config_set_bool)(void* host, const char* key, int value);
} pmc_host_api;

// What a plugin exports. Fill this in and export `pmc_plugin_entry`; the host
// verifies `abi_version`, keeps `plugin_data` and hands it to every callback.
typedef struct pmc_plugin {
  uint32_t struct_size;
  uint32_t abi_version;
  const char* name;        // must match the manifest's `name`
  const char* version;     // free-form, e.g. "1.2.0"
  const char* author;      // may be NULL
  const char* description; // may be NULL

  // Called once with the host table. Register listeners/commands and allocate
  // `*plugin_data` here. A non-zero return refuses the plugin (the host logs
  // it and unloads the library).
  int (*on_load)(const pmc_host_api* api, void** plugin_data);
  // Called once after every plugin in the directory has loaded. Plugins are
  // enabled in dependency order (a manifest's `depends` entries first), so a
  // plugin may rely on the plugins it depends on already being enabled; it
  // must not rely on the order of unrelated peers.
  void (*on_enable)(void* plugin_data);
  // Called once before unload; release file handles, flush, stop threads.
  void (*on_disable)(void* plugin_data);
  // Called once after on_disable, immediately before the library is closed.
  void (*on_unload)(void* plugin_data);
} pmc_plugin;

// Export this symbol. The loader looks it up with dlsym; the manifest's
// optional `entry` key can name another one.
PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PLUSMC_PLUGIN_H
