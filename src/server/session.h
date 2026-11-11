// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-connection session (Phase 1 play slice, protocol 777 / 26.3). Entry
// point for every accepted connection: reads the handshake and routes to the
// status exchange, then the login -> configuration -> play flow. Login runs
// online by default (premium-only; pass --offline-mode for local/LAN testing)
// or through the RSA encryption handshake and Yggdrasil verification; play is
// a minimal loop
// (join game into a void world with a spawn chunk batch, keep-alives, movement
// accepted, chat echoed between connected players).
//
// Interim by design (see docs/PHASE1.md): single canonical version,
// thread-per-connection, writes serialized through one global lock.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "net/tcp_server.h"
#include "server/client_config.h"
#include "server/hosting.h"
#include "world/anvil.h"
#include "world/chunk_generation.h"

namespace mc::world {
class NameListBiomeTable;
}

namespace mc::net {
struct ComponentNode;
}

namespace mc::server {

struct SessionSettings {
  std::string version_name;
  int protocol = 777;
  std::string motd;
  int max_players = 20;
  int compression_threshold = 256; // -1 disables (vanilla default is 256)
  int view_distance = 8;
  int simulation_distance = 8;
  int gamemode = 0; // 0 survival, 1 creative
  // Difficulty (net.minecraft.world.Difficulty ordinal): 0 peaceful, 1 easy,
  // 2 normal, 3 hard. It gates starvation damage and the peaceful "food never
  // drains" rule (FoodData.tick), and is sent to the client at join so its own
  // notion of the difficulty matches (ClientboundChangeDifficultyPacket).
  // Vanilla's server.properties defaults to easy; this server defaults to
  // normal, which is what the vanilla client itself assumes with no packet.
  uint8_t difficulty = 2;
  // Join Game's "is flat" bit: true for the void world and superflat worlds,
  // false for regular overworlds (set via --no-flat-world when serving one).
  bool flat_world = true;
  // Join Game's `hashedSeed`: vanilla sends `BiomeManager.obfuscateSeed(
  // level.getSeed())`, not the seed (`ServerPlayer.createCommonSpawnInfo`). The
  // client feeds it to its own biome manager for the sample it takes outside
  // loaded chunks, so a client told a different number than the terrain was
  // built from can disagree with the server about a biome near the view edge.
  // 0 is what a world with no seed to advertise (an Anvil directory whose
  // level.dat is not read here) keeps sending.
  int64_t hashed_seed = 0;
  // Online (premium) mode: RSA encryption handshake + Yggdrasil verification
  // during login. On by default (premium-only) — pass --offline-mode to
  // disable (e.g. for local testing or LAN without auth).
  bool online_mode = true;
  // Server-initiated Login Plugin Request channels (modded handshakes / proxy
  // protocols): one request per channel is sent during login before Set
  // Compression and its response awaited. Empty = no exchange (vanilla-like).
  std::vector<std::string> login_plugin_channels;
  // Phase 5 ordered item 7 — proxy forwarding. Both off by default: a server
  // that is not behind a proxy must not trust either one (the legacy form is
  // client-controlled, and the modern form only means something with a secret
  // both ends know). See server/proxy.h for the two wire formats.
  //
  // `proxy_legacy_forwarding`: accept BungeeCord's NUL-separated handshake
  // address (`<host>\0<client ip>\0<uuid>[\0<properties>]`) and take the
  // identity from it. A handshake without the extra data is treated as a
  // direct connection (which is what `ip_forward: false` proxies send).
  bool proxy_legacy_forwarding = false;
  // `proxy_velocity_secret`: Velocity modern forwarding, enabled by a non-empty
  // secret. The identity arrives in the signed `velocity:player_info` login
  // plugin response and replaces whatever Login Start claimed.
  std::string proxy_velocity_secret;
  double spawn_x = 8.0;
  double spawn_y = 100.0;
  double spawn_z = 8.0;
  // Real-client smoke hook: after a translated client enters play, force-open
  // the enchanting-table and anvil screens so the opt-in harness can prove a
  // real 26.1/26.2 client decodes them, not just the synthetic wire client.
  bool test_t0_menus = false;
  // Two-client real-client smoke hook: log the translated/canonical player
  // pair in both directions, teleport the translated player out of tracking
  // range and back, and log each state. The harness asserts the pair booleans
  // while both real clients stay connected.
  bool test_t0_visibility = false;
  // The single dimension this process serves. The generated Nether and End
  // modes set `minecraft:nether` / `minecraft:the_end`; a disk-backed world
  // remains the overworld.
  std::string dimension = "minecraft:overworld";
  int sea_level = 63;
  // Play-state keep-alive cadence and unacked-keep-alive timeout, in ms
  // (vanilla-like: 10000 / 15000). Both are env-tunable for tests
  // (PLUSMC_KEEPALIVE_MS / PLUSMC_KEEPALIVE_TIMEOUT_MS).
  int keep_alive_ms = 10000;
  int keep_alive_timeout_ms = 15000;
  // How the session's survival clock counts. A real client sends one packet per
  // game tick, so "any recognized serverbound packet advances the clock by the
  // wall time since the last one" is the 20 Hz player tick — but it also turns a
  // test's *idle* seconds into game ticks, which makes anything measured in
  // ticks (an effect's duration, an item cooldown) a function of how loaded the
  // machine is. PLUSMC_SURVIVAL_CLOCK=packet counts packets instead: exactly one
  // tick each and no catch-up, so idle wall time contributes nothing and a test
  // that sends N ticks gets N ticks. Every test that reaches the play state sets
  // it (status_ping and login_plugin never leave the login states and so have no
  // survival clock to pin); a real session keeps the wall clock.
  bool survival_clock_packet = false;
  // Chunk delivery budget: chunks written per vanilla tick (50 ms) before the
  // send loop pauses. Vanilla's server sends `desiredChunksPerTick` chunks and
  // then waits for the client's ServerboundChunkBatchReceivedPacket (the
  // client's ChunkBatchSizeCalculator, which starts at 3.5 and adapts to how
  // long it actually takes to parse/light/mesh a chunk, clamped to 3x growth).
  // Handing a client an entire view distance in one tick makes it parse, light
  // and mesh thousands of sections back to back — it sits on "Loading terrain"
  // far longer and stutters afterwards. We pace by a fixed budget rather than
  // the reported rate (see docs/PHASE3.md); env-tunable for tests
  // (PLUSMC_CHUNKS_PER_TICK).
  int chunks_per_tick = 9;
  // Interest management (Phase 5 ordered item 3): how far away another player
  // still tracks this one as an in-world entity. This is vanilla's
  // `EntityType.PLAYER.clientTrackingRange()`, in blocks — a viewer outside it
  // is sent no entity packet for that player at all, and is told to remove the
  // entity, the way `ChunkMap.TrackedEntity` does. Runtime rather than a
  // constant so one binary can be measured both ways: a soak that compares a
  // mover's fan-out needs the gate and the no-gate arm from the *same* build on
  // the same box, with only whether a far viewer is sent the relay changing
  // (PLUSMC_PLAYER_TRACKING_RANGE; a range wider than the world is the
  // pre-slice behaviour, where every player relayed to every other).
  double player_tracking_range = 48.0;
  // The range at which tracking *stops*, deliberately wider than the range that
  // starts it. The relay re-evaluates a pair on the mover's own movement packet —
  // vanilla re-evaluates when the tracked entity's chunk section changes — so
  // without a margin a player hovering on the 48-block line spawns and despawns
  // once per packet. This is the equivalent protection for the finer-grained
  // check; set to `player_tracking_range` for vanilla's exact single-threshold
  // rule (PLUSMC_PLAYER_TRACKING_HYSTERESIS=0, the A/B arm).
  double player_tracking_drop_range = 56.0;
  ClientConfigData client_config; // registry listings + tags for our version
  // 1.21.11's configuration registries/tags.  Kept separate: the canonical
  // 26.3 packets cannot be relabelled as 1.21.11 data (the registry set and
  // wire order differ).  Empty unless data/vanilla/1.21.11/clientconfig exists.
  ClientConfigData t1_client_config;
  // 26.1–26.1.2 (protocol 775) and 26.2 (776) configuration assets.  Their
  // synchronized registry sets and ids differ from 26.3 and from each other.
  // Protocol 775 spans three releases with distinct tags, so the list is
  // ordered oldest-first and the client's Known Packs answer selects one.
  std::vector<ClientConfigData> t0_775_client_configs;
  ClientConfigData t0_776_client_config;
  // Hosting-panel files (Phase 2 milestone F).
  // White-list gate: when enabled, only names (or uuids) listed in
  // whitelist.json may join. Vanilla sets `white-list` in
  // server.properties; `enforce-whitelist` kicks online players that are
  // removed from the list, which we do not model yet.
  bool whitelist_enabled = false;
  std::vector<PlayerListEntry> whitelist;
  // Operator list (ops.json). When require_op_for_commands is set, the
  // gameplay commands from milestone D are restricted to operators; an empty
  // ops.json leaves it false so a fresh world stays administrable.
  bool require_op_for_commands = false;
  std::vector<PlayerListEntry> ops;
  int op_permission_level = 4;
  // Phase-2 milestone A: when set, the spawn chunk batch streams real chunks
  // from this Anvil world (chunks missing on disk fall back to the void
  // emitter). Null = the Phase-1 void world. Shared across connections.
  std::shared_ptr<mc::world::WorldInterface> world;
  // Ahead-of-view generation (world/chunk_generation.h): the shared pool that
  // builds the columns a session is about to stream, so the session thread
  // sends instead of generating. Null = the engine's original inline path, which
  // is exactly the `PLUSMC_GEN_THREADS=0` arm of the A/B.
  std::shared_ptr<mc::world::ChunkGeneration> generation;
  // Advertised worldgen/biome registry order (from client_config); the chunk
  // encoder resolves biome names to these wire ids.
  std::vector<std::string> biome_registry;
  // The encoder's lookup built from `biome_registry`, once per server instead
  // of once per chunk. A generated column is encoded on its way to every new
  // viewer, and the table's constructor copies every name and fills a hash map;
  // rebuilding that inside `chunk_body_for` was ~70 allocations per column per
  // viewer, on the join-storm path. main.cpp installs it after the registry is
  // chosen; a caller that leaves it null (most tests) still gets the registry
  // built inline, so the settings stay self-sufficient.
  std::shared_ptr<const mc::world::NameListBiomeTable> biome_table;
};

// Runs one connection to completion (returns on disconnect / protocol error).
void serve_connection(mc::net::Connection& conn, const SessionSettings& settings);

// Number of players currently in play (registry size; thread-safe).
int online_player_count();

// A copy of one registered player's state, for callers that cannot hold the
// registry lock (the plugin host's `player_get`). The registry's `Player` is
// static for the life of a connection, so a snapshot is exactly what a reader
// needs: the live session keeps mutating the entry, and a copy cannot dangle.
struct PlayerSnapshot {
  std::string name;
  uint8_t uuid[16]{};
  double x = 0;
  double y = 0;
  double z = 0;
  float yaw = 0;
  float pitch = 0;
  uint8_t gamemode = 0;
  float health = 20.0f;
  float absorption = 0.0f;
  float max_health = 20.0f;
  uint8_t food_level = 20;
  float saturation = 5.0f;
  float exhaustion = 0.0f;
  int32_t experience_level = 0;
  float experience_progress = 0.0f;
  int32_t experience_points = 0;
  int32_t entity_id = 0;
  // The observed stance delta in blocks per tick (see `pmc_player`), copied
  // under the same registry lock as the position it was measured from.
  double vx = 0.0;
  double vy = 0.0;
  double vz = 0.0;
};

// Looks a player up by name (case-insensitive) among those in play and copies
// its snapshot. False when nobody of that name is connected. Thread-safe.
bool lookup_player_snapshot(const std::string& name, PlayerSnapshot* out);

// The outcome of a plugin's `world_set_block` (pluginapi/plugin.h's
// `enum pmc_block_result`, but kept out of the plugin header's namespace so
// the server does not depend on the ABI).
enum class BlockWriteResult : int {
  Ok = 0,
  NoWorld = -1,
  OutOfRange = -2,
  UnknownState = -3,
  Failed = -4
};

// Applies a block-state name at world coordinates through the engine's own
// block-change path — the same world write, client broadcast, neighbour fan-out
// and redstone settlement `/setblock` performs. `state` is a canonical name
// with optional `[key=value,...]` properties. Called by the plugin host on a
// session or tick thread; the world and the player registry serialise their
// own state. Thread-safe.
BlockWriteResult plugin_set_block(int32_t x, int32_t y, int32_t z, const std::string& state);

// The block-state network id at world coordinates, the read half of
// `plugin_set_block` and what a WASM plugin's `world_get_block` answers. -1 when
// the server has no world, the cell is outside it, or the state is not in the
// generated registry. Thread-safe.
int32_t plugin_get_block(int32_t x, int32_t y, int32_t z);

// One live entity, as the plugin host's `entities` lists it. A snapshot copied
// under the owning registry's lock, with no handle to anything live.
struct PluginEntity {
  int32_t entity_id = 0;
  int32_t type_id = 0;
  std::string type; // registry name, "minecraft:zombie"
  std::string name; // display name, item name for a drop, player name
  double x = 0, y = 0, z = 0;
  float health = 0.0f; // 0 for an entity with no health pool
  int32_t age = 0;
  // Delta movement in blocks per tick. A player's is the observed stance delta
  // between its last two position packets; a cloud has no motion of its own.
  double vx = 0.0, vy = 0.0, vz = 0.0;
  // Look angles in degrees. 0 where the kind does not track rotation; a mob has
  // yaw only, and a projectile's pair is the rotation its velocity implies.
  float yaw = 0.0f, pitch = 0.0f;
};

// Every entity currently in the world: players, mobs, dropped items, orbs,
// primed TNT, projectiles, thrown potions and their clouds. False when the
// server has no world registered; the list is otherwise replaced whole.
bool plugin_list_entities(std::vector<PluginEntity>* out);

// The outcome of an entity write (pluginapi/plugin.h's `enum pmc_entity_result`,
// kept out of the ABI namespace so the server does not depend on the header).
enum class EntityResult : int {
  Ok = 0,
  NotFound = -1,
  Unsupported = -2,
  Invalid = -3,
  Refused = -4
};

// `plugin_get_entity`: the single-entity read beside the listing. The same
// snapshot fields, taken from the one registry that owns the id rather than
// walking all of them. NotFound when no live entity has that id (or the server
// has no world); Invalid for a non-positive id or a null `out`. Thread-safe.
EntityResult plugin_get_entity(int32_t entity_id, PluginEntity* out);

// `plugin_find_entity`: the by-name lookup beside the by-id read. The order is
// fixed and documented in pluginapi/plugin.h: players by account name, then
// mobs by display or registry name, then dropped items by item name, then
// experience orbs, primed TNT, projectiles, thrown potions and area-effect
// clouds by display or type name. The first match wins; NotFound when nothing
// carries the name and Invalid for a null/empty one. Thread-safe.
EntityResult plugin_find_entity(const std::string& name, PluginEntity* out);

// `plugin_entity_despawn`: discard one entity by id. The registry entry leaves
// immediately, every client tracking it is sent Remove Entities, and the
// tracking relationship is dropped. No death event, loot or experience: this is
// `Entity.discard`, not a kill. Players answer Unsupported. Thread-safe.
EntityResult plugin_entity_despawn(int32_t entity_id);

// `plugin_entity_damage`: hurt one living entity by id. A mob is hurt on this
// thread through the same `hurt_mob` a sword hit uses and its viewers are sent
// the damage event, hurt animation and health word; a lethal hit leaves the
// death, drops and experience to the world tick. A player's hit is queued for
// their own session (the `/damage <player>` path), so Ok means accepted, not
// completed. Non-living entities answer Unsupported. Thread-safe.
EntityResult plugin_entity_damage(int32_t entity_id, float amount);

// `plugin_entity_teleport`: move one entity by id and zero its velocity. The
// registry write and the position-sync packet are synchronous; tracking is
// re-evaluated by the next tick's sweep. A player's move is queued for their own
// session, so Ok means accepted. Thread-safe.
EntityResult plugin_entity_teleport(int32_t entity_id, double x, double y, double z);

// One player-inventory stack, returned by `plugin_inventory_get`.
struct PluginItem {
  int32_t item = 0;
  int32_t count = 0;
  int32_t damage = 0;
  int32_t max_stack_size = 0;
  std::string name;
};

// The outcome of the plugin inventory accessors (pluginapi/plugin.h's
// `enum pmc_inventory_result`, kept out of the ABI namespace so the server does
// not depend on the plugin header).
enum class InventoryResult : int {
  Ok = 0,
  NoPlayer = -1,
  OutOfRange = -2,
  UnknownItem = -3,
  WriteFailed = -4
};

// `plugin_inventory_get`: copies one slot of the named player's inventory from
// the snapshot their session published on its last processed packet. Slots are
// the player inventory's own (0..8 hotbar, 9..35 main, 36..39 armour feet-first,
// 40 offhand). Thread-safe.
InventoryResult plugin_inventory_get(const std::string& player, int slot, PluginItem* out);

// `plugin_inventory_set`: validates an item name and count, then queues the
// write for the named player's own session, which applies it on its next tick
// through the same path a menu click takes (inventory write, menu revision
// advance, client slot or equipment update). Returns Ok for a queued write.
// Thread-safe.
InventoryResult plugin_inventory_set(const std::string& player, int slot, const std::string& item,
                                     int count);

// Advances the overworld world clock by `delta_ticks` and broadcasts the new
// time (SetTime 0x71) to every player in play. Called by the server tick loop
// in main.cpp (vanilla: every tick, broadcast every 20).
void advance_world_time(int64_t delta_ticks);

// Current overworld time in ticks (day 0 starts at tick 0 = dawn).
int64_t world_time_ticks();

// The plugin API's world clock and weather (docs/PLUGIN_API.md, slice 16). The
// time setter is the same path as `/time set` — the clock moves and every
// player gets the SetTime update in the call; it refuses a negative tick. The
// weather reader answers 0 clear / 1 rain / 2 thunder (the mode that was set,
// not the ramped level), and the setter is `/weather`'s own parameters,
// including the mode's default duration when `duration_ticks` is <= 0. All are
// thread-safe; false means the argument was refused.
bool plugin_set_world_time(int64_t ticks);
int plugin_world_weather();
bool plugin_set_world_weather(int weather, int32_t duration_ticks);

// The outcome of the plugin messaging writers (pluginapi/plugin.h's
// `enum pmc_message_result`, kept out of the ABI namespace like
// `InventoryResult`).
enum class MessageResult : int { Ok = 0, NoPlayer = -1, Invalid = -2 };

// The plugin API's player messaging (docs/PLUGIN_API.md, slice 17). Both write
// one SystemChat (0x79) line through the same component path the engine's own
// chat and command feedback take, so a plugin's line is ordered with the
// engine's on that connection. `text` is the whole line: one plain component,
// 1..4096 bytes. Thread-safe: the player registry write is serialised and a
// player found under it cannot disconnect before the write.
//
// The direct message answers Ok when it went out, NoPlayer when no player of
// that name is in play and Invalid for an empty or oversized line; the
// broadcast answers its recipient count (>= 0), or Invalid (-2) for a line it
// refused.
MessageResult plugin_player_message(const std::string& player, const std::string& text,
                                    bool overlay = false);
// A rich component keeps its style tree and still uses the same SystemChat
// channel; `overlay` selects its action-bar flag. The plain overload is the
// common case and is exactly this with a one-node tree.
MessageResult plugin_player_message(const std::string& player, const mc::net::ComponentNode& text,
                                    bool overlay = false);
int plugin_broadcast_message(const std::string& text);

// The outcome of a plugin's player-state write (pluginapi/plugin.h's
// `enum pmc_player_result`, kept out of the ABI namespace like
// `InventoryResult`).
enum class PlayerStateResult : int {
  Ok = 0,
  NoPlayer = -1,
  Unsupported = -2,
  Invalid = -3,
  Refused = -4
};

// The plugin API's player-state writes (docs/PLUGIN_API.md, slice 18). Each
// validates its argument range, resolves the name (case-insensitive) and queues
// one `PendingPlayerAction` for that session, which applies it on its next
// packet tick through the engine's own path: the game mode through
// `apply_gamemode`, health and food through the survival state and the
// `set_health` guard. Ok therefore means *accepted*, not completed; NoPlayer
// means nobody of that name is in play (including a player who left between
// the lookup and the queue) and Invalid a refused argument. A queued health or
// food write for a dead player is consumed without reviving them.
// Thread-safe.
PlayerStateResult plugin_player_set_gamemode(const std::string& player, int gamemode);
PlayerStateResult plugin_player_set_health(const std::string& player, float health);
PlayerStateResult plugin_player_set_food(const std::string& player, int food, float saturation);

// Registers the world the server tick loop advances. main.cpp owns the world
// (it is what opened level.dat and streams the chunks), so it hands the same
// shared_ptr to every session *and* registers it here: the furnace tick is a
// server-wide job, not a per-session one. Passing nullptr unregisters.
void set_server_world(std::shared_ptr<mc::world::WorldInterface> world);

// The `randomTickSpeed` gamerule (vanilla's default 3), which is how many
// precipitation samples `ServerLevel.tickChunk` draws per chunk per tick.
// main.cpp sets it from PLUSMC_RANDOM_TICK_SPEED, so a test can make the sample
// dense enough to observe without waiting out vanilla's one-in-48 gate.
void set_random_tick_speed(int speed);

// The natural-spawning switch (vanilla's `SPAWN_MOBS` gamerule). main.cpp sets
// it from PLUSMC_MOB_SPAWNING; the pass is on by default, and off means no
// `NaturalSpawner` work at all (no state, no candidate chunks).
void set_natural_spawning(bool enabled);

// The natural spawn pass's own account, for the shutdown path: the mobs it
// created, the passes that walked their candidates, and the candidate chunks
// asked of the world — tickable against refused. Empty when the pass never ran
// (spawning off, or no player ever in play), so a run without spawning does not
// print a zero line. Called once at shutdown, like the tracking reports; the
// counters are atomic and are still moving while sessions drain.
std::string natural_spawn_report();

// `LevelData.RespawnData`: the world spawn point, published by main.cpp from
// the world it opened (level.dat, or the defaults for a world without one). The
// natural spawn pass keeps 24 blocks around it clear, exactly as
// `isRightDistanceToPlayerAndSpawnPoint` does.
void set_server_spawn(double x, double y, double z);

// The level's weather for persistence: level.dat's four fields (`rainTime`,
// `thunderTime`, `raining`, `thundering`) and the ramped levels (written as
// +MC's own `rainLevel` / `thunderLevel`), so a restart resumes a fade where it
// stopped rather than snapping to full strength.
struct SavedWeather {
  int rain_time = 0;
  int thunder_time = 0;
  bool raining = false;
  bool thundering = false;
  float rain_level = 0.0f;
  float thunder_level = 0.0f;
};
SavedWeather saved_weather();
void restore_weather(const SavedWeather& weather);

// Runs `delta_ticks` game ticks of world simulation: today the furnace family
// (fuel burn, cook progress, the `lit` property, the experience owed). Called
// by main.cpp's loop, and independent of the day/night cycle (a test that
// freezes the clock still wants a furnace to cook).
void tick_world(int64_t delta_ticks);

// The mob population's save file (`<world_dir>/mobs.dat`, world/mob_save.h):
// where the entity ids of every live mob and the animal state that references
// them (a baby's parent, a breeding pair's partner, the love countdown) survive
// a restart, so a goal that holds a mob id does not die with the process.
//
// `load` must run before the listener starts (it replaces the population), and
// `save` may run from the tick loop's autosave and on shutdown; both are
// thread-safe against the tick thread (the copy happens under the mob lock) and
// return false only on I/O problems. A world with no file yet loads as an empty
// population. Only a disk-backed world has a save file: the void and `--worldgen`
// worlds live for the process.
bool load_mob_population(const std::filesystem::path& world_dir);
bool save_mob_population(const std::filesystem::path& world_dir);

// The write path's deterministic number (docs/WRITE_PATH.md): of every chunk
// send this process made, how many reused a body another connection had built
// and how many reused a whole compressed frame. Empty when nothing was sent,
// so a shutdown of a server nobody joined does not print a zero line. Called
// once from the shutdown path; the counters are atomic and may still be
// moving while a session thread drains.
std::string chunk_share_report();

// Entity tracking — the same interest management (Phase 5's ordered item 3
// covers players) applied to the entity relays: a mob's or a dropped item's
// packets go only to the clients inside its tracking range, and the transitions
// send the pairing burst and Remove Entities the way vanilla's
// `ChunkMap.TrackedEntity` does. The two ranges are server-wide rather than in
// `SessionSettings` because the per-tick sweep runs on the tick thread, which
// has no session's settings to read; main.cpp publishes them once at boot.
//
// `range_override` 0 selects each type's own vanilla `clientTrackingRange * 16`
// (world/entity_tracking_table.h); a positive value replaces every kind's range
// (PLUSMC_ENTITY_TRACKING_RANGE, with the mob-era PLUSMC_MOB_TRACKING_RANGE
// alias) for the A/B and the red check. `hysteresis` widens the drop line past
// the entry line. `view_blocks` is the viewer's view distance in blocks: vanilla
// clamps every type's range to it, so a range that reaches past the streamed
// chunks still only pairs inside them. `enabled` false is the pre-slice arm
// (PLUSMC_ENTITY_VISIBILITY=0, or its PLUSMC_MOB_VISIBILITY alias): every
// tracked entity to every player, in the same binary.
void set_entity_tracking(double range_override, int hysteresis, double view_blocks, bool enabled);

// The load-independent numbers the entity relays report on shutdown: how many
// payloads the tick built, how many client writes they became, how many
// tracking relationships started, and the largest viewer set an entity ever had.
// Each line is empty when no payload of that kind was ever built, so a server
// with no mobs (or no drops) does not print a zero line. Called once from the
// shutdown path, like `chunk_share_report`.
std::string mob_tracking_report();
std::string item_tracking_report();
// ...and the later relays on the same map and the same line shape: experience
// orbs, primed TNT, arrows/thrown items, thrown potions and their clouds.
std::string orb_tracking_report();
std::string tnt_tracking_report();
std::string projectile_tracking_report();
std::string potion_tracking_report();
std::string cloud_tracking_report();

// (The persistence section continues above with the stats and scoreboard
// stores; this is the advancement one.)
// The advancement store's save file (`<world_dir>/advancements.dat`,
// world/advancement_save.h): every player's advancement progress and known
// recipes, online or not — a registry entry lives only as long as its session,
// so this is what makes the tree survive a reconnect and a restart both. A join
// adopts the player's rows into the fresh entry, a disconnect copies them back
// out, and these two move the whole store to and from disk.
//
// `load` must run before the listener starts (it replaces the store), and `save`
// may run from the tick loop's autosave and on shutdown; both are thread-safe
// against sessions (the copy happens under the player registry lock). A world
// with no file yet loads as an empty store, and only a disk-backed world has a
// save file at all. Returns false only on I/O or structural problems.
bool load_advancement_state(const std::filesystem::path& world_dir);
bool save_advancement_state(const std::filesystem::path& world_dir);

// The stats store's save file (`<world_dir>/stats.dat`, world/stats_save.h):
// every player's stat counters, online or not. A join adopts the player's rows
// into the fresh entry, a disconnect copies them back out, and these two move
// the whole store to and from disk. As with the advancement store, `load` must
// run before the listener starts and `save` may run from the autosave and the
// shutdown; both are thread-safe against sessions.
bool load_stats_state(const std::filesystem::path& world_dir);
bool save_stats_state(const std::filesystem::path& world_dir);

// The scoreboard's save file (`<world_dir>/scoreboard.dat`,
// world/scoreboard_save.h): objectives, scores, display slots and teams. The
// board is server-wide, so there is one board to write, not one per player.
// `load` must run before the listener starts; `save` may run from the autosave
// and the shutdown.
bool load_scoreboard_state(const std::filesystem::path& world_dir);
bool save_scoreboard_state(const std::filesystem::path& world_dir);

} // namespace mc::server
