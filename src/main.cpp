// SPDX-License-Identifier: GPL-3.0-or-later
//
// +MC server entry point. Phase 1 slice: server list ping (status) on the
// canonical version. The advertised version/protocol come from the vanilla
// data extracted by tools/vanilla-data (data-driven, docs/PLAN.md §7) with a
// CLI override for environments without extracted data.
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h defines these as macros unless NOMINMAX was set before it was first
// included. The entry point uses std::min/std::max below.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <atomic>
#endif

#include "core/json_util.h"
#include "core/log.h"
#include "core/metrics.h"
#include "core/page_alloc.h"
#include "core/slab_pool.h"
#include "modapi/mod_package.h"
#include "net/tcp_server.h"
#include "pluginapi/plugin_host.h"
#include "server/backup.h"
#include "server/client_config.h"
#include "server/proxy.h"
#include "server/session.h"
#include "server/wasm_plugins.h"
#include "world/anvil.h"
#include "world/directory_lock.h"
#include "world/memory_world.h"
#include "world/registry.h"
#include "world/world_light.h"
#include "world/worldgen_chunk.h"
#include "world/worldgen_surface.h"

namespace fs = std::filesystem;

namespace logging = mc::log; // logging lives in mc::log; alias it in the global namespace

namespace {

using mc::core::json_int_field;
using mc::core::json_string_field;

struct Config {
  std::string host = "0.0.0.0";
  uint16_t port = 25565;
  std::string motd = "A +MC server";
  int max_players = 20;
  // Version advertisement; resolved from vanilla data unless overridden.
  std::string version_name;
  int protocol = -1;
  fs::path data_root = "data/vanilla";
  // Join/game tuning (defaults match vanilla-ish settings).
  int compression_threshold = 256;
  int view_distance = 8;
  int simulation_distance = 8;
  int gamemode = 0;
  bool online_mode = true;
  // Login Plugin Request channels (repeatable): a server-initiated plugin
  // handshake before Set Compression (modded/proxy compatibility); see
  // SessionSettings::login_plugin_channels.
  std::vector<std::string> login_plugin_channels;
  // Phase-5 ordered item 7 — proxy forwarding. `bungeecord` is the legacy
  // handshake form (Spigot's own name for the setting); `velocity_forwarding`
  // enables the modern, HMAC-signed form and needs `velocity_secret`.
  bool bungeecord = false;
  bool velocity_forwarding = false;
  std::string velocity_secret;
  // Phase-2 milestone A: serve an existing Anvil world instead of the
  // Phase-1 void world. Empty = void world (default).
  fs::path world_dir;
  // Join Game "is flat"; true matches the void/superflat worlds, false a
  // regular overworld (--no-flat-world when serving one).
  bool flat_world = true;
  // Test hook for tests/real_client_smoke_test.py: force-open the translated
  // enchanting-table and anvil menus after a T0 client enters play.
  bool test_t0_menus = false;
  // Test hook for the two-client arm of the same test: log and exercise the
  // translated/canonical player pair across tracking-range transitions.
  bool test_t0_visibility = false;
  // --worldgen: build the overworld from this seed with the in-tree generator
  // (noise, biomes and surface rules) instead of serving a world directory or
  // the void column.
  std::optional<int64_t> worldgen_seed;
  // --dimension: which table --worldgen builds. The End's noise column is 128
  // high, but its level stem is 256-high; the decorated bridge extends it.
  std::string worldgen_dimension = "overworld";
  // Hosting-panel files (Phase-2 milestone F) live in this directory:
  // server.properties / eula.txt / whitelist.json / ops.json. Defaults to the
  // working directory (where a host launches the server), overridable with
  // --config-dir.
  fs::path config_dir = ".";
  bool accept_eula = false;
  // Seeded from server.properties, enforced during join/commands.
  bool whitelist_enabled = false;
  int op_permission_level = 4;
  // --metrics-interval: seconds between tick-metrics log lines (0 = off). The
  // line is the only place the loop's own cost is visible; docs/PHASE5.md's
  // numbers and bench/soak.py both read it. See core/metrics.h.
  int metrics_interval_s = 0;
  // --metrics-file: append each reported window to this file as one JSON object
  // per line (the metrics *export* — a consumer for the numbers the stderr line
  // already carries; docs/PHASE5.md §6 item 8). Empty = no file.
  fs::path metrics_file;
  // --plugins: a directory of native plugins, each `<plugin>/+mc-plugin.toml`
  // (docs/PLUGIN_API.md). Empty = none, which is the default: loading code into
  // the server is the operator's explicit choice.
  fs::path plugins_dir;
  // --wasm-plugins: a directory of core-module WASM packages, each
  // `<plugin>/+mc-wasm.toml` and the module it names. Loaded at boot with the
  // native plugins; docs/PHASE8.md owns the gate, budgets and host surface.
  fs::path wasm_plugins_dir;
  // --mods: a directory of signed `.pmcmod` packages. Unlike --plugins, the
  // packages are verified, extracted into a cache and then handed to the same
  // native plugin host. Empty = none. `--mod-key` is repeatable; unsigned
  // packages require the explicit development flag.
  fs::path mods_dir;
  std::vector<fs::path> mod_keys;
  fs::path mods_cache;
  bool allow_unsigned_mods = false;
  // --validate-config: dry-run the effective configuration, report it, and exit
  // without starting the server (docs/PHASE5.md §6 item 8's other half).
  bool validate_config_only = false;
  // Phase-5 ops (docs/PHASE5.md §6 item 8): snapshots of the world's persistent
  // state, their rotation, and the restore drill. `backup_dir` defaults to
  // <world>/backups when a world directory is served (and stays empty for the
  // void/--worldgen worlds, which have nothing on disk to snapshot).
  // `backup_interval_s` 0 = no scheduled pass; `--restore-backup <name>` is a
  // one-shot startup restore that runs before the world is opened.
  fs::path backup_dir;
  int backup_keep = 5;
  int backup_interval_s = 0;
  std::string restore_backup;
};

void print_usage() {
  std::printf("+MC server (Phase 1: status/ping slice)\n"
              "\n"
              "Usage: plusmc [options]\n"
              "  --host <ip>          Bind address, dotted IPv4 (default 0.0.0.0)\n"
              "  --port <n>           TCP port; 0 picks a free port (default 25565)\n"
              "  --motd <text>        Status description (default \"A +MC server\")\n"
              "  --max-players <n>    Advertised player cap (default 20)\n"
              "  --data <dir>         Root holding data/vanilla/<version>/ extracts from\n"
              "                       tools/vanilla-data; newest version.json wins\n"
              "                       (default data/vanilla)\n"
              "  --protocol <n>       Override advertised protocol version\n"
              "  --version-name <s>   Override advertised version name\n"
              "  --online-mode       Require authentication through Mojang's session\n"
              "                       server (encryption + Yggdrasil; default on)\n"
              "  --offline-mode      Disable authentication (offline/cracked mode;\n"
              "                       no session-server check, offline UUIDs)\n"
              "  --login-plugin-request <channel>\n"
              "                       Server-initiated Login Plugin Request (repeatable;\n"
              "                       modded/proxy handshakes). Default: none\n"
              "  --bungeecord        Trust legacy BungeeCord forwarding: the proxy\n"
              "                       appends the client's IP/UUID/profile to the\n"
              "                       handshake address. Only for a backend that is not\n"
              "                       reachable by clients directly (default off)\n"
              "  --velocity-forwarding\n"
              "                       Accept Velocity modern forwarding (the signed\n"
              "                       velocity:player_info login plugin response); needs\n"
              "                       --velocity-secret (default off)\n"
              "  --velocity-secret <s>\n"
              "                       The secret shared with the Velocity proxy\n"
              "  --world <dir>       Serve the existing Anvil world at <dir> (26.3 layout:\n"
              "                       dimensions/minecraft/overworld/region). Spawn chunks\n"
              "                       stream from disk; chunks missing from disk fall back\n"
              "                       to the void emitter. Default: the void world\n"
              "  --worldgen <seed>   Generate a world from <seed> (a 64-bit integer)\n"
              "  --dimension <name>  With --worldgen: overworld (default), nether or\n"
              "                       end with the built-in noise world generator; the\n"
              "                       spawn point is searched from the terrain. Mutually\n"
              "                       exclusive with --world\n"
              "  --gamemode <n>      Default gamemode: 0=Survival 1=Creative 2=Adventure\n"
              "                       3=Spectator (default 0)\n"
              "  --view-distance <n> Chunk radius streamed around a player (1..32, default 8)\n"
              "  --no-flat-world     Join Game reports a regular overworld (not flat); use\n"
              "                       when --world serves a normal overworld\n"
              "  --test-t0-menus     Force-open the translated enchanting-table and\n"
              "                       anvil menus for real-client smoke tests\n"
              "  --test-t0-visibility\n"
              "                       Log and exercise translated/canonical player\n"
              "                       pairing across tracking range for smoke tests\n"
              "  --config-dir <dir>  Directory holding server.properties / eula.txt /\n"
              "                       whitelist.json / ops.json (default: cwd). CLI flags\n"
              "                       override server.properties values\n"
              "  --accept-eula       Start even without eula=true in eula.txt (the EULA\n"
              "                       gate applies when a hosting config is present)\n"
              "  --metrics-interval <s>\n"
              "                       Log a tick-metrics line every <s> seconds (0 =\n"
              "                       off, default 0; env PLUSMC_METRICS_SECS). The\n"
              "                       line's key=value pairs are what bench/soak.py\n"
              "                       and docs/PHASE5.md parse\n"
              "  --metrics-file <path>\n"
              "                       Append each reported metrics window to <path> as\n"
              "                       one JSON object per line (same numbers as the\n"
              "                       metrics line; a host can tail or forward it).\n"
              "                       Turns the interval on at 1 s when none was given\n"
              "  --validate-config   Do not start: check the effective configuration\n"
              "                       (CLI flags over server.properties), print every\n"
              "                       problem as `config: error`/`config: warning`,\n"
              "                       and exit 0 when there is none (warnings alone do\n"
              "                       not fail) or 1 when there is\n"
              "  --backup-dir <dir>  Where world snapshots are written and rotation\n"
              "                       looks for them (default: <world>/backups when\n"
              "                       --world is served; snapshots need a world on\n"
              "                       disk, so the void/--worldgen worlds have none)\n"
              "  --backup-keep <n>   Snapshots to keep, newest first (default 5; 0\n"
              "                       keeps every one)\n"
              "  --backup-interval <s>\n"
              "                       Flush and snapshot the world every <s> seconds\n"
              "                       (0 = off, default 0; env PLUSMC_BACKUP_SECS).\n"
              "                       Each pass logs `backup: wrote <path>`\n"
              "  --restore-backup <name>\n"
              "                       Restore <name> from --backup-dir over --world at\n"
              "                       startup, before the world is opened, then serve\n"
              "                       it (the restore drill; refuses anything that is\n"
              "                       not one of this server's own snapshots)\n"
              "  --plugins <dir>     Load the native plugins in <dir>: every\n"
              "                       subdirectory with a +mc-plugin.toml (or <dir>\n"
              "                       itself when it is one plugin). A plugin that\n"
              "                       fails to load is logged and skipped; see\n"
              "                       docs/PLUGIN_API.md\n"
              "  --wasm-plugins <dir> Load core-module WASM plugins from <dir>: every\n"
              "                       subdirectory with a +mc-wasm.toml (or <dir> itself).\n"
              "                       A package that fails the gate or a budget is\n"
              "                       logged and skipped; see docs/PHASE8.md\n"
              "  --mods <dir>        Verify signed .pmcmod packages in <dir>, extract\n"
              "                       them into a cache and load their native plugins\n"
              "  --mod-key <pem>     Trusted Ed25519 public key for --mods (repeatable)\n"
              "  --mods-cache <dir>  Extracted package cache (default:\n"
              "                       <config-dir>/mod-cache)\n"
              "  --allow-unsigned-mods\n"
              "                       Permit unsigned .pmcmod packages (development only)\n"
              "  --debug             Verbose protocol logging\n"
              "  -h, --help           Show this help\n");
}

// Maps a server.properties gamemode token (survival/creative/adventure/
// spectator, case-insensitive) to its numeric id, or -1 when unknown.
int gamemode_from_property(const std::string& raw) {
  std::string v;
  v.reserve(raw.size());
  for (char c : raw)
    v.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (v == "survival" || v == "0")
    return 0;
  if (v == "creative" || v == "1")
    return 1;
  if (v == "adventure" || v == "2")
    return 2;
  if (v == "spectator" || v == "3")
    return 3;
  return -1;
}

// Seeds Config from server.properties so CLI flags can override it (the same
// precedence a host expects: edit the file, or pass a flag for one run).
// `properties_present` reports whether the file existed (the EULA gate keys
// off it).
Config load_config_defaults(const fs::path& dir, bool* properties_present) {
  Config cfg;
  cfg.config_dir = dir;
  mc::server::ServerProperties props = mc::server::ServerProperties::load(dir);
  *properties_present = props.present();
  if (!props.present())
    return cfg;

  if (auto v = props.get_int("server-port", cfg.port)) {
    // 0 is the documented "pick a free port" value (the same one --port
    // accepts). Treating it as invalid silently left the default 25565 in
    // place, so a test or dev boot that asked for an ephemeral port could
    // collide with another server while reporting no error.
    if (v >= 0 && v <= 65535)
      cfg.port = static_cast<uint16_t>(v);
  }
  if (auto v = props.get("motd"))
    cfg.motd = *v;
  if (auto v = props.get_int("max-players", cfg.max_players)) {
    if (v >= 0 && v <= 1000000)
      cfg.max_players = v;
  }
  cfg.online_mode = props.get_bool("online-mode", cfg.online_mode);
  cfg.bungeecord = props.get_bool("bungeecord", cfg.bungeecord);
  cfg.velocity_forwarding = props.get_bool("velocity-forwarding", cfg.velocity_forwarding);
  if (auto v = props.get("velocity-secret"))
    cfg.velocity_secret = *v;
  cfg.whitelist_enabled = props.get_bool("white-list", false);
  if (auto v = props.get_int("op-permission-level", cfg.op_permission_level)) {
    if (v >= 1 && v <= 4)
      cfg.op_permission_level = v;
  }
  if (auto v = props.get("gamemode")) {
    const int gm = gamemode_from_property(*v);
    if (gm >= 0)
      cfg.gamemode = gm;
  }
  if (auto v = props.get_int("view-distance", cfg.view_distance)) {
    if (v >= 1 && v <= 32) {
      cfg.view_distance = v;
      cfg.simulation_distance = v;
    }
  }
  if (auto v = props.get_int("network-compression-threshold", cfg.compression_threshold)) {
    if (v >= -1 && v <= 1 << 20)
      cfg.compression_threshold = v;
  }
  // level-name points at the world dir under the server root (vanilla default
  // "world"). Adopt it even when the directory does not exist yet: a hosting
  // config always names a world, and AnvilWorld::open_world bootstraps the
  // 26.3 layout (vanilla generates the level too). A `--world` flag still wins,
  // and without a server.properties there is no level-name at all, so a bare
  // dev run keeps the in-memory world.
  {
    std::string level_name = "world";
    if (auto v = props.get("level-name"); v && !v->empty())
      level_name = *v;
    cfg.world_dir = dir / level_name;
  }
  return cfg;
}

// JSON helpers live in core/json_util.h (shared with client_config.cpp).

// Picks the extract with the highest data version under data_root and fills
// in version_name/protocol. Returns true when vanilla data was found.
bool resolve_version_from_data(const fs::path& root, Config* cfg) {
  std::error_code ec;
  int64_t best_dv = -1;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    if (ec || !entry.is_directory()) {
      ec.clear();
      continue;
    }
    fs::path vj = entry.path() / "version.json";
    if (!fs::exists(vj))
      continue;
    std::ifstream in(vj);
    if (!in)
      continue;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto dv = json_int_field(text, "world_version");
    auto proto = json_int_field(text, "protocol_version");
    auto name = json_string_field(text, "id");
    if (!dv || !proto || !name)
      continue;
    if (*dv > best_dv) {
      best_dv = *dv;
      cfg->version_name = *name;
      cfg->protocol = static_cast<int>(*proto);
    }
  }
  if (best_dv < 0)
    return false;
  logging::info("advertising ", cfg->version_name, " (protocol ", cfg->protocol, ", data version ",
                best_dv, ") from ", (root / cfg->version_name).string());
  return true;
}

// Strict integer parsing: the whole string must be a valid decimal number in
// [lo, hi]. (strtol's end pointer is never nullptr on input, so checking it
// alone silently accepts garbage like "abc" as 0 — this helper closes that.)
std::optional<long> parse_clamped_int(const std::string& s, long lo, long hi) {
  char* end = nullptr;
  errno = 0;
  long v = std::strtol(s.c_str(), &end, 10);
  if (errno == ERANGE || end == s.c_str() || *end != '\0' || v < lo || v > hi)
    return std::nullopt;
  return v;
}

std::optional<uint16_t> parse_port(const std::string& s) {
  auto v = parse_clamped_int(s, 0, 65535);
  if (!v)
    return std::nullopt;
  return static_cast<uint16_t>(*v);
}

// A world seed is the full signed 64-bit range (the seed stack hashes it), so
// this is `strtoll` rather than `parse_clamped_int`.
std::optional<int64_t> parse_seed(const std::string& s) {
  if (s.empty())
    return std::nullopt;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno == ERANGE || end == s.c_str() || *end != '\0')
    return std::nullopt;
  return static_cast<int64_t>(v);
}

Config parse_args(int argc, char** argv, Config cfg) {
  std::vector<std::string> args(argv + 1, argv + argc);
  bool world_flag = false;
  bool worldgen_flag = false;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= args.size()) {
        logging::error(what, " requires a value");
        std::exit(2);
      }
      return args[++i];
    };
    if (a == "--host") {
      cfg.host = next("--host");
    } else if (a == "--port") {
      auto p = parse_port(next("--port"));
      if (!p) {
        logging::error("invalid --port");
        std::exit(2);
      }
      cfg.port = *p;
    } else if (a == "--motd") {
      cfg.motd = next("--motd");
    } else if (a == "--max-players") {
      auto v = parse_clamped_int(next("--max-players"), 0, 1000000);
      if (!v) {
        logging::error("invalid --max-players (expected 0..1000000)");
        std::exit(2);
      }
      cfg.max_players = static_cast<int>(*v);
    } else if (a == "--data") {
      cfg.data_root = next("--data");
    } else if (a == "--protocol") {
      auto v = parse_clamped_int(next("--protocol"), 0, 2147483647);
      if (!v) {
        logging::error("invalid --protocol (expected 0..2147483647)");
        std::exit(2);
      }
      cfg.protocol = static_cast<int>(*v);
    } else if (a == "--version-name") {
      cfg.version_name = next("--version-name");
    } else if (a == "--online-mode") {
      cfg.online_mode = true;
    } else if (a == "--offline-mode") {
      cfg.online_mode = false;
    } else if (a == "--world") {
      cfg.world_dir = next("--world");
      world_flag = true;
    } else if (a == "--worldgen") {
      auto v = parse_seed(next("--worldgen"));
      if (!v) {
        logging::error("invalid --worldgen seed (expected a 64-bit integer)");
        std::exit(2);
      }
      cfg.worldgen_seed = *v;
      worldgen_flag = true;
    } else if (a == "--dimension") {
      cfg.worldgen_dimension = next("--dimension");
      if (cfg.worldgen_dimension != "overworld" && cfg.worldgen_dimension != "nether" &&
          cfg.worldgen_dimension != "end") {
        logging::error("--dimension must be overworld, nether or end");
        std::exit(2);
      }
    } else if (a == "--view-distance") {
      auto v = parse_clamped_int(next("--view-distance"), 1, 32);
      if (!v) {
        logging::error("invalid --view-distance (expected 1..32)");
        std::exit(2);
      }
      cfg.view_distance = static_cast<int>(*v);
      cfg.simulation_distance = static_cast<int>(*v);
    } else if (a == "--gamemode") {
      auto v = parse_clamped_int(next("--gamemode"), 0, 3);
      if (!v) {
        logging::error(
            "invalid --gamemode (expected 0=Survival 1=Creative 2=Adventure 3=Spectator)");
        std::exit(2);
      }
      cfg.gamemode = static_cast<int>(*v);
    } else if (a == "--no-flat-world") {
      cfg.flat_world = false;
    } else if (a == "--test-t0-menus") {
      cfg.test_t0_menus = true;
    } else if (a == "--test-t0-visibility") {
      cfg.test_t0_visibility = true;
    } else if (a == "--config-dir") {
      cfg.config_dir = next("--config-dir");
    } else if (a == "--accept-eula") {
      cfg.accept_eula = true;
    } else if (a == "--login-plugin-request") {
      cfg.login_plugin_channels.push_back(next("--login-plugin-request"));
    } else if (a == "--bungeecord") {
      cfg.bungeecord = true;
    } else if (a == "--velocity-forwarding") {
      cfg.velocity_forwarding = true;
    } else if (a == "--velocity-secret") {
      cfg.velocity_secret = next("--velocity-secret");
    } else if (a == "--metrics-interval") {
      auto v = parse_clamped_int(next("--metrics-interval"), 0, 86400);
      if (!v) {
        logging::error("invalid --metrics-interval (expected 0..86400 seconds)");
        std::exit(2);
      }
      cfg.metrics_interval_s = static_cast<int>(*v);
    } else if (a == "--metrics-file") {
      cfg.metrics_file = next("--metrics-file");
    } else if (a == "--plugins") {
      cfg.plugins_dir = next("--plugins");
    } else if (a == "--wasm-plugins") {
      cfg.wasm_plugins_dir = next("--wasm-plugins");
    } else if (a == "--mods") {
      cfg.mods_dir = next("--mods");
    } else if (a == "--mod-key") {
      cfg.mod_keys.emplace_back(next("--mod-key"));
    } else if (a == "--mods-cache") {
      cfg.mods_cache = next("--mods-cache");
    } else if (a == "--allow-unsigned-mods") {
      cfg.allow_unsigned_mods = true;
    } else if (a == "--validate-config") {
      cfg.validate_config_only = true;
    } else if (a == "--backup-dir") {
      cfg.backup_dir = next("--backup-dir");
    } else if (a == "--backup-keep") {
      auto v = parse_clamped_int(next("--backup-keep"), 0, 1000000);
      if (!v) {
        logging::error("invalid --backup-keep (expected 0..1000000; 0 keeps every snapshot)");
        std::exit(2);
      }
      cfg.backup_keep = static_cast<int>(*v);
    } else if (a == "--backup-interval") {
      auto v = parse_clamped_int(next("--backup-interval"), 0, 86400);
      if (!v) {
        logging::error("invalid --backup-interval (expected 0..86400 seconds)");
        std::exit(2);
      }
      cfg.backup_interval_s = static_cast<int>(*v);
    } else if (a == "--restore-backup") {
      cfg.restore_backup = next("--restore-backup");
    } else if (a == "--debug") {
      mc::log::set_level(mc::log::Level::Debug);
    } else if (a == "-h" || a == "--help") {
      print_usage();
      std::exit(0);
    } else {
      logging::error("unknown argument '", a, "' (see --help)");
      std::exit(2);
    }
  }
  if (world_flag && worldgen_flag) {
    logging::error("--world and --worldgen are mutually exclusive: --world serves an existing "
                   "world directory, --worldgen builds one from a seed");
    std::exit(2);
  }
  if (!worldgen_flag && cfg.worldgen_dimension != "overworld") {
    logging::error("--dimension requires --worldgen: it selects the generated dimension, "
                   "not the dimension of a disk-backed world");
    std::exit(2);
  }
  // A hosting config always names a level (`level-name`, default "world"), so
  // load_config_defaults has already put a disk path in world_dir. That path is
  // the --world mode's world; a generated world is in memory and has no disk
  // backing (so neither backups nor saves apply). Clearing it here keeps the
  // two modes from looking mutually exclusive to --validate-config, which
  // would otherwise reject every configured `--worldgen` host.
  if (worldgen_flag)
    cfg.world_dir.clear();
  return cfg;
}

bool is_mod_package(const fs::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension == ".pmcmod";
}

// The immediate `.pmcmod` children of a --mods directory, in a deterministic
// order. An unreadable directory is an error, not an empty package set.
std::vector<fs::path> mod_packages(const fs::path& dir, std::error_code* ec) {
  std::vector<fs::path> packages;
  ec->clear();
  for (fs::directory_iterator it(dir, *ec), end; !*ec && it != end; it.increment(*ec)) {
    std::error_code type_error;
    if (it->is_regular_file(type_error) && is_mod_package(it->path()))
      packages.push_back(it->path());
  }
  std::sort(packages.begin(), packages.end());
  return packages;
}

mc::modapi::VerifyOptions mod_verify_options(const Config& cfg) {
  mc::modapi::VerifyOptions options;
  options.trusted_keys = cfg.mod_keys;
  options.allow_unsigned = cfg.allow_unsigned_mods;
  return options;
}

// Verify each package, then extract it under a content-addressed cache. A bad
// package is logged and skipped; only a bad --mods path or cache directory is
// fatal. The marker keeps a later boot from trusting a half-written cache.
bool prepare_mods(const Config& cfg, std::vector<fs::path>* roots, std::string* fatal) {
  if (cfg.mods_dir.empty())
    return true;
  std::error_code ec;
  if (!fs::is_directory(cfg.mods_dir, ec)) {
    *fatal = "mods: " + cfg.mods_dir.string() + " is not a directory";
    return false;
  }
  const fs::path cache = cfg.mods_cache.empty() ? cfg.config_dir / "mod-cache" : cfg.mods_cache;
  fs::create_directories(cache, ec);
  if (ec) {
    *fatal = "mods: cannot create cache " + cache.string() + ": " + ec.message();
    return false;
  }

  const std::vector<fs::path> packages = mod_packages(cfg.mods_dir, &ec);
  if (ec) {
    *fatal = "mods: cannot read " + cfg.mods_dir.string() + ": " + ec.message();
    return false;
  }
  if (packages.empty())
    logging::warn("mods: no .pmcmod packages in ", cfg.mods_dir.string());

  const mc::modapi::VerifyOptions verify_options = mod_verify_options(cfg);
  std::size_t failed = 0;
  for (const fs::path& package : packages) {
    const mc::modapi::VerifyReport report = mc::modapi::verify_package(package, verify_options);
    if (!report.ready) {
      logging::error("mods: ", package.filename().string(), ": ", report.reason);
      ++failed;
      continue;
    }

    const fs::path destination = cache / report.archive_sha256;
    const fs::path marker = destination / ".plusmc-verified";
    std::string cached_hash;
    {
      std::ifstream in(marker, std::ios::binary);
      std::getline(in, cached_hash);
    }
    const bool reusable = cached_hash == report.archive_sha256 &&
                          fs::is_regular_file(destination / "mod.json", ec) &&
                          fs::is_regular_file(destination / "+mc-plugin.toml", ec);
    if (!reusable) {
      fs::remove_all(destination, ec);
      mc::modapi::ExtractOptions extract_options;
      extract_options.verify = verify_options;
      const mc::modapi::ExtractReport extracted =
          mc::modapi::extract_package(package, destination, extract_options);
      if (!extracted.extracted) {
        logging::error("mods: ", package.filename().string(), ": ", extracted.error);
        ++failed;
        continue;
      }
      std::ofstream out(marker, std::ios::binary | std::ios::trunc);
      out << report.archive_sha256 << "\n";
      if (!out)
        logging::warn("mods: cannot write cache marker ", marker.string());
    }
    roots->push_back(destination);
    logging::info("mods: verified ", report.id, " ", report.version, " (", report.signature,
                  ") from ", package.filename().string());
  }
  logging::info("mods: ", roots->size(), " ready, ", failed, " refused from ",
                cfg.mods_dir.string());
  return true;
}

// ---- Phase-5 ops: `--validate-config`, the dry run --------------------------
//
// A host that edits server.properties on a live box wants to know the file is
// sane *before* the restart takes the server down, and the startup path is the
// only thing that knows what "sane" means: it clamps ranges, gates on the EULA,
// resolves the world directory and runs the restore drill. The validator is a
// read-only walk of the same questions — it never creates a directory, never
// writes an eula.txt and never opens the world — and it reports each answer as
// either
//
//   * an **error** — startup would fail, or would serve something the operator
//     did not ask for (a port out of range silently falls back to 25565, which
//     is exactly the misconfiguration this exists to catch), or
//   * a **warning** — startup proceeds, but check it anyway (a world directory
//     that does not exist yet is created; a missing data root still starts on
//     the fallback protocol).
//
// The check list is deliberately the loaders' own set, not a second grammar:
// server.properties' known keys and their vanilla ranges, the EULA gate, the
// world directory, the backup/restore targets, the login-plugin channels, the
// metrics file and the vanilla-data/client-config assets.
struct ConfigIssue {
  bool error = false;
  std::string text;
};

std::vector<ConfigIssue> validate_config(const Config& cfg, bool properties_present) {
  std::vector<ConfigIssue> issues;
  const auto err = [&issues](std::string text) {
    issues.push_back(ConfigIssue{true, std::move(text)});
  };
  const auto warn = [&issues](std::string text) {
    issues.push_back(ConfigIssue{false, std::move(text)});
  };
  std::error_code ec;

  // CLI-level invariants. parse_args refuses these with exit(2), so a validator
  // reached through some other path should say the same thing rather than
  // assume the argv it was handed was already checked.
  if (!cfg.world_dir.empty() && cfg.worldgen_seed)
    err("--world and --worldgen are mutually exclusive (--world serves a directory, "
        "--worldgen builds one from a seed)");
  if (!cfg.worldgen_seed && cfg.worldgen_dimension != "overworld")
    err("--dimension requires --worldgen (it selects the generated dimension)");

  // The hosting-config directory itself: every file below is read relative to
  // it, and the EULA gate keys off it, so a path that is not there is an error
  // even though a server without a hosting config is a legitimate dev run.
  if (!fs::exists(cfg.config_dir, ec))
    err("config directory " + cfg.config_dir.string() + " does not exist");
  else if (!fs::is_directory(cfg.config_dir, ec))
    err("config directory " + cfg.config_dir.string() + " is not a directory");

  // server.properties: the keys the loader reads, checked against the same
  // ranges it clamps to. The loader silently substitutes its default when a
  // value does not parse or is out of range, and the substituted value is what
  // the server then binds/advertises — the operator's intent is lost without a
  // word, which is why this is an error rather than a warning.
  if (properties_present) {
    mc::server::ServerProperties props = mc::server::ServerProperties::load(cfg.config_dir);
    const auto check_range = [&](const char* key, long lo, long hi) {
      const auto raw = props.get(key);
      if (!raw)
        return;
      if (!parse_clamped_int(*raw, lo, hi)) {
        err(std::string("server.properties: ") + key + "='" + *raw + "' is not an integer in " +
            std::to_string(lo) + ".." + std::to_string(hi) +
            "; the loader would fall back to its default");
      }
    };
    check_range("server-port", 0, 65535);
    check_range("max-players", 0, 1000000);
    check_range("view-distance", 1, 32);
    check_range("network-compression-threshold", -1, 1 << 20);
    check_range("op-permission-level", 1, 4);
    for (const char* key : {"online-mode", "white-list"}) {
      const auto raw = props.get(key);
      if (!raw)
        continue;
      const std::string v = [&] {
        std::string s = *raw;
        for (char& c : s)
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
          s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
          s.pop_back();
        return s;
      }();
      if (v != "true" && v != "false" && v != "1" && v != "0" && v != "yes" && v != "no")
        err(std::string("server.properties: ") + key + "='" + *raw +
            "' is not a boolean; the loader would fall back to its default");
    }
    if (const auto gm = props.get("gamemode"); gm && gamemode_from_property(*gm) < 0)
      err("server.properties: gamemode='" + *gm +
          "' is not survival/creative/adventure/spectator (or 0..3); the loader would "
          "fall back to its default");
    if (const auto level = props.get("level-name"); level && level->empty())
      warn("server.properties: level-name is empty; the default 'world' is used");
  }

  // The EULA gate the runtime enforces (read-only here: --validate-config must
  // not bootstrap eula.txt the way a real first start does).
  const bool eula_file = fs::exists(cfg.config_dir / "eula.txt");
  if (!cfg.accept_eula && (properties_present || eula_file)) {
    if (mc::server::read_eula(cfg.config_dir) != mc::server::EulaState::Accepted)
      err("eula.txt is not accepted (set eula=true in " + (cfg.config_dir / "eula.txt").string() +
          ", or pass --accept-eula)");
  } else if (cfg.accept_eula && !properties_present && !eula_file) {
    warn("--accept-eula has nothing to accept: no server.properties and no eula.txt in " +
         cfg.config_dir.string());
  }

  // A host never wants an ephemeral port: 0 is the dev/test value that makes
  // the server bind a free one and log which.
  if (cfg.port == 0)
    warn("port 0 picks a free port at startup — right for a dev run, not for a host");

  // The world. The runtime creates the directory and bootstraps the 26.3 layout
  // (world/anvil.h), so a missing one is a warning, not an error — but a path
  // that exists as something that is not a directory can never become a world.
  if (cfg.worldgen_seed) {
    // --worldgen keeps its world in memory; there is nothing on disk to check.
  } else if (!cfg.world_dir.empty()) {
    if (fs::exists(cfg.world_dir, ec) && !fs::is_directory(cfg.world_dir, ec)) {
      err("--world " + cfg.world_dir.string() + " exists but is not a directory");
    } else if (!fs::exists(cfg.world_dir, ec)) {
      warn("--world " + cfg.world_dir.string() +
           " does not exist yet; startup creates it as a fresh, empty world");
    } else if (!fs::exists(cfg.world_dir / "level.dat")) {
      warn("--world " + cfg.world_dir.string() +
           " has no level.dat; its spawn point falls back to the built-in default and "
           "vanilla tools may not recognise it");
    }
  }

  // Snapshots need a world on disk; the backup pass is a no-op without one.
  const fs::path backup_dir = cfg.backup_dir.empty() ? cfg.world_dir / "backups" : cfg.backup_dir;
  if (cfg.backup_interval_s > 0 && cfg.world_dir.empty())
    warn("--backup-interval is set but there is no --world: nothing on disk to snapshot");

  // The restore drill's target, validated with the same predicate the drill
  // itself uses (server/backup.h) — a name that is not a snapshot must be
  // refused at the config stage rather than after the world has been replaced.
  if (!cfg.restore_backup.empty()) {
    if (cfg.world_dir.empty()) {
      err("--restore-backup needs a world to restore into (--world)");
    } else if (cfg.restore_backup == "." || cfg.restore_backup == ".." ||
               cfg.restore_backup.find('/') != std::string::npos ||
               cfg.restore_backup.find('\\') != std::string::npos) {
      err("--restore-backup '" + cfg.restore_backup +
          "' is a path, not a snapshot name (one directory name, no separators)");
    } else {
      const fs::path snapshot = backup_dir / cfg.restore_backup;
      std::string why;
      if (!fs::is_directory(snapshot, ec))
        err("--restore-backup " + cfg.restore_backup + ": no snapshot at " + snapshot.string());
      else if (!mc::server::is_backup_dir(snapshot, &why))
        err("--restore-backup " + cfg.restore_backup + ": not a usable backup (" + why + ")");
    }
  }

  // --metrics-file: the parent has to exist (the runtime fails to open it and
  // exits) and the path must not be a directory.
  if (!cfg.metrics_file.empty()) {
    if (fs::is_directory(cfg.metrics_file, ec)) {
      err("--metrics-file " + cfg.metrics_file.string() + " is a directory");
    } else if (const fs::path parent = cfg.metrics_file.parent_path();
               !parent.empty() && !fs::is_directory(parent, ec)) {
      err("--metrics-file: the directory " + parent.string() + " does not exist");
    }
  }

  // --plugins: the directory itself. A typo must not silently start a server
  // with none of the plugins the operator believes are running.
  if (!cfg.plugins_dir.empty() && !fs::is_directory(cfg.plugins_dir, ec))
    err("--plugins " + cfg.plugins_dir.string() + " is not a directory");

  // --wasm-plugins has the same typo rule: an operator who asked for a
  // directory of WASM plugins must not silently get none of them.
  if (!cfg.wasm_plugins_dir.empty() && !fs::is_directory(cfg.wasm_plugins_dir, ec))
    err("--wasm-plugins " + cfg.wasm_plugins_dir.string() + " is not a directory");

  // --mods has the same typo rule. A package that fails verification is a
  // package-level failure, not a startup failure: the server still starts
  // with the packages it could verify.
  if (!cfg.mods_dir.empty()) {
    if (!fs::is_directory(cfg.mods_dir, ec)) {
      err("--mods " + cfg.mods_dir.string() + " is not a directory");
    } else if (std::vector<fs::path> packages = mod_packages(cfg.mods_dir, &ec); !ec) {
      if (packages.empty())
        warn("--mods " + cfg.mods_dir.string() + " contains no .pmcmod packages");
      for (const fs::path& package : packages) {
        const mc::modapi::VerifyReport report =
            mc::modapi::verify_package(package, mod_verify_options(cfg));
        if (!report.ready)
          err("--mods " + package.filename().string() + ": " + report.reason);
      }
    } else {
      err("cannot read --mods " + cfg.mods_dir.string() + ": " + ec.message());
    }
  }
  for (const fs::path& key : cfg.mod_keys) {
    if (!fs::is_regular_file(key, ec))
      err("--mod-key " + key.string() + " is not a file");
  }
  if (!cfg.mods_cache.empty() && fs::is_regular_file(cfg.mods_cache, ec))
    err("--mods-cache " + cfg.mods_cache.string() + " is a file");

  // Login Plugin Request channels are namespaced (`velocity:player_info`); a
  // channel the client cannot resolve is a handshake that hangs, not a typo the
  // server can recover from.
  for (const std::string& channel : cfg.login_plugin_channels) {
    if (channel.empty() || channel.find(':') == std::string::npos)
      err("--login-plugin-request '" + channel + "' is not a namespaced channel (namespace:path)");
  }

  // Proxy forwarding (item 7). Velocity's modern form is a shared-secret
  // protocol: enabling it without the secret leaves the server unable to verify
  // anything, and asking for its channel through the generic plugin knob would
  // both send it twice and bypass the identity substitution forwarding exists
  // to perform.
  if (cfg.velocity_forwarding && cfg.velocity_secret.empty())
    err("--velocity-forwarding needs --velocity-secret (the value from the proxy's config)");
  if (!cfg.velocity_forwarding && !cfg.velocity_secret.empty())
    warn("velocity-secret is set but velocity-forwarding is off; the secret is unused");
  if (cfg.velocity_forwarding && !cfg.velocity_secret.empty()) {
    for (const std::string& channel : cfg.login_plugin_channels) {
      if (channel == mc::server::kVelocityPlayerInfoChannel)
        err(std::string("--login-plugin-request ") + channel +
            " duplicates the Velocity forwarding exchange (--velocity-forwarding already sends "
            "it)");
    }
  }
  // The proxy authenticates the client, and the proxy->backend link is plain,
  // so forwarding plus the backend's own online-mode check are two answers to
  // one question. Warn rather than error: a host may be forwarding into a
  // server that also serves direct connections.
  if ((cfg.bungeecord || cfg.velocity_forwarding) && cfg.online_mode)
    warn("proxy forwarding is enabled with online-mode=true; the proxy authenticates the "
         "client, so a forwarded backend normally runs with online-mode=false");

  // Vanilla data: the advertised version/protocol come from it, and the client
  // config assets are what lets a client join at all. Warning, not error — the
  // runtime already starts on the fallback protocol and says so.
  Config discovered = cfg;
  if (!resolve_version_from_data(cfg.data_root, &discovered) &&
      (cfg.protocol < 0 || cfg.version_name.empty()))
    warn("no usable vanilla data under " + cfg.data_root.string() +
         "; the fallback protocol is advertised (run tools/vanilla-data)");
  else if (!mc::server::load_client_config(cfg.data_root.string()).ok())
    warn("no client config assets under " + (cfg.data_root / "clientconfig").string() +
         " (data root has evidence)");

  return issues;
}

int run_validate_config(const Config& cfg, bool properties_present) {
  logging::info("validating configuration (config dir ", cfg.config_dir.string(), ", data ",
                cfg.data_root.string(), ", protocol ",
                cfg.protocol >= 0 ? std::to_string(cfg.protocol) : "auto", ")");
  int errors = 0, warnings = 0;
  for (const ConfigIssue& issue : validate_config(cfg, properties_present)) {
    if (issue.error) {
      ++errors;
      // The level is in the message as well as in the log tag: a host greps
      // the line, not the tag, and `config: error: ...` is the contract.
      logging::error("config: error: ", issue.text);
    } else {
      ++warnings;
      logging::warn("config: warning: ", issue.text);
    }
  }
  if (errors > 0) {
    logging::error("config: ", errors, " error(s), ", warnings,
                   " warning(s) — the server would not start as configured");
    return 1;
  }
  logging::info("config: ok (", warnings, " warning(s))");
  return 0;
}

// The spawn of a generated world, standing in for vanilla's `setInitialSpawn`
// search: walk chunks in rings around the origin and take the first column
// whose motion-blocking surface is dry land — above the noise stage's sea
// level, with a block that is not a fluid under it. Seed 42's origin chunk is
// open ocean, so the default column is not good enough. Each chunk the search
// reaches is materialised (the player streams it anyway) and read from its own
// live column. If the whole square is water, the caller gets a spawn just above
// the sea at the origin instead of the old mid-air default.
//
// Whether a biome is open water at sea level.  The spawn scan uses this as a
// cheap pre-filter: generating an ocean chunk just to learn its top block is
// water costs a full noise/surface column (~15 ms on an idle box), while the
// biome source answers the same question in a fraction of that.
bool spawn_biome_is_water(const std::string& biome) {
  static const char* kWaterBiomes[] = {
      "minecraft:ocean",           "minecraft:deep_ocean",     "minecraft:cold_ocean",
      "minecraft:deep_cold_ocean", "minecraft:lukewarm_ocean", "minecraft:deep_lukewarm_ocean",
      "minecraft:warm_ocean",      "minecraft:frozen_ocean",   "minecraft:deep_frozen_ocean",
      "minecraft:river",
  };
  for (const char* name : kWaterBiomes) {
    if (biome == name)
      return true;
  }
  return false;
}

std::optional<std::tuple<int, int, int>> generated_nether_spawn(mc::world::MemoryWorld& world) {
  // The Nether has a bedrock roof, so `chunk_motion_blocking_height` would put
  // the player on top of it. Search below the roof for a solid floor with two
  // air blocks above and no lava in the footing.
  for (int radius = 0; radius <= 16; ++radius) {
    for (int cx = -radius; cx <= radius; ++cx) {
      for (int cz = -radius; cz <= radius; ++cz) {
        if (std::max(std::abs(cx), std::abs(cz)) != radius)
          continue;
        const auto borrowed = world.modify_chunk(cx, cz);
        if (!borrowed)
          continue;
        mc::world::Chunk& chunk = borrowed->get();
        for (int lz = 0; lz < 16; ++lz) {
          for (int lx = 0; lx < 16; ++lx) {
            for (int y = 100; y > 33; --y) {
              const mc::world::BlockStateKey floor = chunk.block_state_at(lx, y - 1, lz);
              if (floor.name == "minecraft:air" || floor.name == "minecraft:lava" ||
                  floor.name == "minecraft:bedrock")
                continue;
              if (chunk.block_state_at(lx, y, lz).name == "minecraft:air" &&
                  chunk.block_state_at(lx, y + 1, lz).name == "minecraft:air")
                return std::make_tuple(cx * 16 + lx, y, cz * 16 + lz);
            }
          }
        }
      }
    }
  }
  return std::make_tuple(8, 64, 8);
}

std::optional<std::tuple<int, int, int>> generated_world_spawn(mc::world::MemoryWorld& world) {
  // Vanilla keeps looking until it finds dry land, and some seeds put the
  // origin in open ocean (seed 1 is deep ocean for its first ~150 blocks, with
  // the nearest shore around chunk (6,10)). Six chunks of rings is not enough
  // there: the scan would find nothing and leave the player at the old default
  // height, in mid-air over water. Sixteen chunks (256 blocks) reaches that
  // shore and stays a bounded startup cost.
  for (int radius = 0; radius <= 16; ++radius) {
    for (int cx = -radius; cx <= radius; ++cx) {
      for (int cz = -radius; cz <= radius; ++cz) {
        if (std::max(std::abs(cx), std::abs(cz)) != radius)
          continue; // the ring only; inner chunks were scanned
        // Skip open water without generating the column; the height test below
        // would only reject it afterwards.
        if (spawn_biome_is_water(
                world.biome_name_at(cx * 16 + 8, mc::world::kWgSeaLevel + 1, cz * 16 + 8)))
          continue;
        const auto borrowed = world.modify_chunk(cx, cz);
        if (!borrowed)
          continue;
        mc::world::Chunk& chunk = borrowed->get();
        for (int lz = 0; lz < 16; ++lz) {
          for (int lx = 0; lx < 16; ++lx) {
            const int height = mc::world::chunk_motion_blocking_height(chunk, lx, lz);
            if (height <= mc::world::kWgSeaLevel)
              continue;
            const mc::world::BlockStateKey top = chunk.block_state_at(lx, height - 1, lz);
            if (top.name == "minecraft:water" || top.name == "minecraft:lava")
              continue;
            return std::make_tuple(cx * 16 + lx, height, cz * 16 + lz);
          }
        }
      }
    }
  }
  // A pathological seed with no dry land in range still gets a sane spawn:
  // just above the sea surface at the origin, not the old y=100 default.
  return std::make_tuple(8, mc::world::kWgSeaLevel + 2, 8);
}

} // namespace

// Ctrl-C / SIGTERM request a clean stop; the main loop watches this flag.
//
// The Windows console handler runs on a system thread rather than in signal
// context, so the flag is an atomic there (a `sig_atomic_t` write from another
// thread would be a data race). POSIX keeps `volatile sig_atomic_t`, which is
// the type the standard gives for a signal handler.
#if defined(_WIN32)
std::atomic<bool> g_stop_requested{false};
#else
volatile std::sig_atomic_t g_stop_requested = 0;
#endif

#if defined(_WIN32)
// Ctrl-C, Ctrl-Break, closing the console, logoff and system shutdown all end
// in the same flag the POSIX signals set, so the main loop runs the ordinary
// drain-and-save path. Returning TRUE tells the console the event was handled;
// for close/logoff/shutdown Windows still allows only a bounded grace period
// before terminating the process, which is why the flag is set immediately and
// the handler does no work itself.
BOOL WINAPI handle_console_control(DWORD event) {
  switch (event) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    g_stop_requested.store(true, std::memory_order_relaxed);
    return TRUE;
  default:
    return FALSE;
  }
}
#else
void handle_signal(int) {
  g_stop_requested = 1;
}
#endif

void install_signal_handlers() {
#if defined(_WIN32)
  if (!::SetConsoleCtrlHandler(handle_console_control, TRUE))
    logging::warn("cannot install the console control handler (error ", ::GetLastError(), ")");
#else
  struct sigaction sa{};
  sa.sa_handler = &handle_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
#endif
}

int main(int argc, char** argv) {
  // Pre-scan for --config-dir: server.properties seeds the defaults, so it has
  // to be read before the CLI is parsed (CLI flags then override it).
  fs::path config_dir = ".";
  const std::vector<std::string> raw_args(argv + 1, argv + argc);
  for (size_t i = 0; i + 1 < raw_args.size(); ++i)
    if (raw_args[i] == "--config-dir")
      config_dir = raw_args[i + 1];

  bool properties_present = false;
  Config cfg = load_config_defaults(config_dir, &properties_present);
  cfg = parse_args(argc, argv, cfg);

  // `--validate-config` is a dry run: it reports and exits *here*, above the
  // EULA gate (which bootstraps an eula.txt) and above everything that opens
  // the world or binds a socket. A refused config is exit 1; warnings alone
  // are exit 0, so a CI gate can require the strong form.
  if (cfg.validate_config_only)
    return run_validate_config(cfg, properties_present);

  install_signal_handlers();

  // Hosting-panel EULA gate. Vanilla refuses to start until eula.txt says
  // `eula=true`; the gate is live whenever a hosting config is present (a
  // server.properties or an eula.txt), so a bare CLI/dev run is unaffected.
  // --accept-eula bypasses it for automated hosts.
  if (!cfg.accept_eula && (properties_present || fs::exists(cfg.config_dir / "eula.txt"))) {
    const mc::server::EulaState eula = mc::server::read_eula(cfg.config_dir);
    if (eula != mc::server::EulaState::Accepted) {
      if (eula == mc::server::EulaState::Missing)
        mc::server::write_eula_template(cfg.config_dir);
      logging::error("You must agree to the Minecraft EULA to run this server: set eula=true in ",
                     (cfg.config_dir / "eula.txt").string(),
                     " (or pass --accept-eula). See https://aka.ms/MinecraftEULA");
      return 1;
    }
  }

  // Resolve version advertisement: explicit flags win, else vanilla data.
  if (cfg.protocol < 0 || cfg.version_name.empty()) {
    Config discovered = cfg;
    if (resolve_version_from_data(cfg.data_root, &discovered)) {
      if (cfg.protocol < 0)
        cfg.protocol = discovered.protocol;
      if (cfg.version_name.empty())
        cfg.version_name = discovered.version_name;
    }
  }
  if (cfg.protocol < 0) {
    cfg.protocol = 777; // 26.3; placeholder until data is bundled (docs/PLAN.md §14.3)
    logging::warn("no vanilla data under ", cfg.data_root.string(),
                  " and no --protocol given — advertising fallback protocol ", cfg.protocol,
                  " (run tools/vanilla-data first)");
  }
  if (cfg.version_name.empty())
    cfg.version_name = "unknown";

  mc::server::SessionSettings settings;
  settings.version_name = cfg.version_name;
  settings.protocol = cfg.protocol;
  settings.motd = cfg.motd;
  settings.max_players = cfg.max_players;
  settings.compression_threshold = cfg.compression_threshold;
  settings.view_distance = cfg.view_distance;
  settings.simulation_distance = cfg.simulation_distance;
  settings.gamemode = cfg.gamemode;
  settings.flat_world = cfg.flat_world;
  settings.test_t0_menus = cfg.test_t0_menus;
  settings.test_t0_visibility = cfg.test_t0_visibility;
  settings.online_mode = cfg.online_mode;
  settings.login_plugin_channels = cfg.login_plugin_channels;
  // Phase-5 item 7: proxy forwarding. Legacy forwarding is a switch; Velocity
  // forwarding is on when a secret is configured (a secret with no forwarding
  // has no meaning, and the config validator refuses `--velocity-forwarding`
  // without one).
  settings.proxy_legacy_forwarding = cfg.bungeecord;
  if (cfg.velocity_forwarding)
    settings.proxy_velocity_secret = cfg.velocity_secret;
  if (cfg.bungeecord)
    logging::info("bungeecord: legacy proxy forwarding accepted (only safe behind a firewall)");
  if (!settings.proxy_velocity_secret.empty())
    logging::info("velocity: modern proxy forwarding enabled (identity is HMAC-verified)");
  if (settings.online_mode)
    logging::info("online mode: enabled (authenticating through the Mojang session server)");
  // Keep-alive timing is env-tunable so tests can exercise the cadence and
  // timeout paths without waiting the vanilla 10s/15s.
  if (const char* ka = std::getenv("PLUSMC_KEEPALIVE_MS"))
    if (auto v = parse_clamped_int(ka, 100, 60000))
      settings.keep_alive_ms = static_cast<int>(*v);
  if (const char* kt = std::getenv("PLUSMC_KEEPALIVE_TIMEOUT_MS"))
    if (auto v = parse_clamped_int(kt, 500, 300000))
      settings.keep_alive_timeout_ms = static_cast<int>(*v);
  // PLUSMC_SURVIVAL_CLOCK=packet makes the survival clock count packets rather
  // than wall time (one tick each, no catch-up), so a test that measures tick
  // durations is not at the mercy of its own idle seconds. See session.h.
  if (const char* sc = std::getenv("PLUSMC_SURVIVAL_CLOCK"))
    settings.survival_clock_packet = std::string(sc) == "packet";
  // Chunk-send budget (chunks per 50 ms tick). Tests raise this to keep the
  // suite fast; 0 or a missing value keeps the default.
  if (const char* ct = std::getenv("PLUSMC_CHUNKS_PER_TICK"))
    if (auto v = parse_clamped_int(ct, 1, 1000))
      settings.chunks_per_tick = static_cast<int>(*v);
  // Interest management (Phase 5 ordered item 3): how far another player is
  // tracked as an in-world entity. 48 blocks is vanilla's player tracking
  // range, so the shipped server is unchanged; a value wider than the world is
  // how a measurement runs the *pre-slice* fan-out arm — every player relaying
  // to every other — against the very same binary and box.
  if (const char* tr = std::getenv("PLUSMC_PLAYER_TRACKING_RANGE"))
    if (auto v = parse_clamped_int(tr, 1, 32768))
      settings.player_tracking_range = static_cast<double>(*v);
  // ... and the hysteresis that keeps a pair tracked past that line, in blocks:
  // tracking starts at the range and stops at range + this, so a player hovering
  // on the boundary does not spawn and despawn once per movement packet. 0 is
  // vanilla's exact single-threshold rule. The drop range is derived here rather
  // than left at its default so that a wider tracking range moves the drop line
  // with it (the pre-slice fan-out arm stays a single threshold away).
  int tracking_hysteresis = 8;
  if (const char* hy = std::getenv("PLUSMC_PLAYER_TRACKING_HYSTERESIS"))
    if (auto v = parse_clamped_int(hy, 0, 1024))
      tracking_hysteresis = static_cast<int>(*v);
  settings.player_tracking_drop_range =
      settings.player_tracking_range + static_cast<double>(tracking_hysteresis);
  // Entity tracking — the gate the mob and item relays share
  // (docs/MOB_VISIBILITY.md). Each entity type is tracked at its own vanilla
  // `clientTrackingRange * 16` blocks, clamped to the viewer's view distance
  // (world/entity_tracking_table.h); 0 here means "use the per-type table".
  // PLUSMC_ENTITY_TRACKING_RANGE replaces every kind's own range — the pre-slice
  // fan-out arm and the red check both need that — and PLUSMC_ENTITY_VISIBILITY=0
  // is the fan-out spelled out, so a soak can measure the gate with one binary on
  // one box. The mob-era names are still read as aliases: the checked-in A/B
  // records and the docs' red check use them.
  double entity_tracking_range = 0.0;
  if (const char* er = std::getenv("PLUSMC_ENTITY_TRACKING_RANGE")) {
    if (auto v = parse_clamped_int(er, 1, 32768))
      entity_tracking_range = static_cast<double>(*v);
  } else if (const char* mr = std::getenv("PLUSMC_MOB_TRACKING_RANGE")) {
    if (auto v = parse_clamped_int(mr, 1, 32768))
      entity_tracking_range = static_cast<double>(*v);
  }
  int entity_hysteresis = 8;
  if (const char* eh = std::getenv("PLUSMC_ENTITY_TRACKING_HYSTERESIS")) {
    if (auto v = parse_clamped_int(eh, 0, 1024))
      entity_hysteresis = static_cast<int>(*v);
  } else if (const char* mh = std::getenv("PLUSMC_MOB_TRACKING_HYSTERESIS")) {
    if (auto v = parse_clamped_int(mh, 0, 1024))
      entity_hysteresis = static_cast<int>(*v);
  }
  bool entity_visibility = true;
  if (const char* ev = std::getenv("PLUSMC_ENTITY_VISIBILITY"))
    entity_visibility = std::string(ev) != "0";
  else if (const char* mv = std::getenv("PLUSMC_MOB_VISIBILITY"))
    entity_visibility = std::string(mv) != "0";
  mc::server::set_entity_tracking(entity_tracking_range, entity_hysteresis,
                                  static_cast<double>(settings.view_distance) * 16.0,
                                  entity_visibility);
  // Hosting-panel gates: whitelist + ops. A missing/empty ops.json leaves
  // every player administrable (the fresh-world bootstrap); once ops exist,
  // the gameplay commands become operator-only.
  settings.whitelist_enabled = cfg.whitelist_enabled;
  settings.whitelist = mc::server::load_player_list(cfg.config_dir / "whitelist.json");
  settings.ops = mc::server::load_player_list(cfg.config_dir / "ops.json");
  settings.require_op_for_commands = !settings.ops.empty();
  settings.op_permission_level = cfg.op_permission_level;
  if (settings.whitelist_enabled)
    logging::info("whitelist: enabled (", settings.whitelist.size(), " entries)");
  if (settings.require_op_for_commands)
    logging::info("ops: ", settings.ops.size(), " operators (gameplay commands are op-only)");
  // The plugin API reads the same ops list: a listed operator with no explicit
  // level gets server.properties' default, and an unlisted name answers -1.
  mc::plugins::host().set_op_levels(
      [ops = settings.ops, fallback = settings.op_permission_level](const std::string& name) {
        for (const auto& entry : ops) {
          if (entry.name.size() != name.size())
            continue;
          bool same = true;
          for (std::size_t i = 0; i < name.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(entry.name[i])) !=
                std::tolower(static_cast<unsigned char>(name[i]))) {
              same = false;
              break;
            }
          }
          if (same)
            return std::min(entry.level > 0 ? entry.level : fallback, 4);
        }
        return -1;
      });
  // Slice 16: the world clock and weather. The reads are the tick loop's own
  // clock and the session's weather snapshot; the writes go through the same
  // `broadcast_set_time` and `set_weather_parameters` the `/time` and `/weather`
  // commands use, so a plugin's change is indistinguishable to clients.
  mc::plugins::host().set_world_state(
      [] { return mc::server::world_time_ticks(); },
      [](int64_t ticks) { return mc::server::plugin_set_world_time(ticks) ? 0 : -2; },
      [] { return mc::server::plugin_world_weather(); },
      [](int weather, int32_t duration) {
        return mc::server::plugin_set_world_weather(weather, duration) ? 0 : -2;
      });
  // Slice 17: player messaging. The writers build the same `system_chat` line
  // the engine's own chat and command feedback do, and the count reads the
  // live registry, so a plugin can address any player, not just the one whose
  // event it is in.
  mc::plugins::host().set_messaging(
      [](const std::string& player, const std::string& text) {
        return static_cast<int>(mc::server::plugin_player_message(player, text));
      },
      [](const std::string& text) { return mc::server::plugin_broadcast_message(text); },
      [] { return mc::server::online_player_count(); });
  // Slice 18: player state writes. Each queues the same action its
  // cross-session command queues, so the target's own session applies it
  // through the engine's gamemode/health/food path on its next tick.
  mc::plugins::host().set_player_state(
      [](const std::string& player, int gamemode) {
        return static_cast<int>(mc::server::plugin_player_set_gamemode(player, gamemode));
      },
      [](const std::string& player, float health) {
        return static_cast<int>(mc::server::plugin_player_set_health(player, health));
      },
      [](const std::string& player, int food, float saturation) {
        return static_cast<int>(mc::server::plugin_player_set_food(player, food, saturation));
      });

  settings.client_config = mc::server::load_client_config(cfg.data_root.string());
  if (!settings.client_config.ok())
    logging::warn("no client config assets found under ", cfg.data_root.string(),
                  "/clientconfig — clients will not be able to join (see docs/PHASE1.md)");
  settings.t1_client_config = mc::server::load_client_config(cfg.data_root.string(), "1.21.11");
  if (!settings.t1_client_config.ok())
    logging::warn("no 1.21.11 client config under ", cfg.data_root.string(),
                  "/1.21.11/clientconfig — T1 login/configuration will be refused");
  for (const char* version : {"26.1", "26.1.1", "26.1.2"}) {
    auto config = mc::server::load_client_config(cfg.data_root.string(), version);
    if (config.ok())
      settings.t0_775_client_configs.push_back(std::move(config));
  }
  if (settings.t0_775_client_configs.empty())
    logging::warn("no 26.1 client config under ", cfg.data_root.string(),
                  "/26.1*/clientconfig — 26.1 login/configuration will be refused");
  settings.t0_776_client_config = mc::server::load_client_config(cfg.data_root.string(), "26.2");
  if (!settings.t0_776_client_config.ok())
    logging::warn("no 26.2 client config under ", cfg.data_root.string(),
                  "/26.2/clientconfig — 26.2 login/configuration will be refused");

  // Phase-5 ops: where snapshots live, and the restore drill. The drill runs
  // *before* anything opens the world — a restore that overwrote region files
  // under a live world would be a half-restore — so it sits here, above the
  // open, and its failure is fatal rather than a warning: starting on the world
  // the operator asked to replace is worse than not starting at all.
  if (cfg.backup_dir.empty() && !cfg.world_dir.empty())
    cfg.backup_dir = cfg.world_dir / "backups";
  // The level's exclusive lock (`net/minecraft/util/DirectoryLock`, 26.2):
  // vanilla takes it when a level is opened and checks it before every write.
  // Here it is taken *above* the restore drill, so a restore runs under the same
  // lock the server then holds for its life and no second server can open — or
  // restore into — the world in between. It is also what lets `plusmc-restore`
  // see a live world instead of only warning about one (docs/PHASE5.md §5).
  std::optional<mc::world::DirectoryLock> world_lock;
  if (!cfg.world_dir.empty()) {
    std::string why;
    world_lock = mc::world::DirectoryLock::acquire(cfg.world_dir, &why);
    if (!world_lock) {
      logging::error("world ", cfg.world_dir.string(), " is already in use: ", why);
      logging::error("stop the process holding it, or choose another --world; nothing was "
                     "written");
      return 1;
    }
  }
  if (!cfg.restore_backup.empty()) {
    if (cfg.world_dir.empty()) {
      logging::error("--restore-backup needs a world directory to restore into (--world)");
      return 1;
    }
    const mc::server::BackupResult restored =
        mc::server::restore_backup(cfg.world_dir, cfg.backup_dir, cfg.restore_backup);
    if (!restored.ok) {
      logging::error("restore: ", restored.error);
      return 1;
    }
    logging::info("restore: ", cfg.restore_backup, " -> ", cfg.world_dir.string(), " (",
                  restored.files, " files, ", restored.bytes, " bytes)");
  }

  // The directory this world's save files live in: the mob population (Phase 4:
  // entity ids that survive a restart, world/mob_save.h) and the advancement
  // store (advancement progress and known recipes, world/advancement_save.h).
  // Only a disk-backed `--world` has one; the void and `--worldgen` worlds live
  // for the process and keep no save.
  fs::path save_dir;

  // The in-memory store's ceiling (docs/PHASE5.md §4, "Per-view memory"): how
  // many columns it may keep resident and how many bytes they may add up to.
  // Both dimensions are opt-in and both are env-tunable rather than flags: a
  // *count* of 4096 is ~1.9 GB for full-height overworld columns and a fraction
  // of that for the void mode's air columns, which is exactly the surprise a
  // byte ceiling removes (src/world/memory_world.h's `ChunkBudget`). Zero on
  // either side means "this side is unbounded", so
  // PLUSMC_CHUNK_BUDGET_COLUMNS=0 is how a run measures the unbounded store at
  // the same binary. `PLUSMC_CHUNK_BUDGET_MB` is whole mebibytes.
  auto read_chunk_budget = []() {
    mc::world::ChunkBudget budget;
    if (const char* cols = std::getenv("PLUSMC_CHUNK_BUDGET_COLUMNS")) {
      if (auto v = parse_clamped_int(cols, 0, 1 << 20)) {
        budget.columns = static_cast<std::size_t>(*v);
      } else {
        logging::warn("PLUSMC_CHUNK_BUDGET_COLUMNS='", cols,
                      "' is not an integer in 0..1048576; "
                      "keeping the default ",
                      budget.columns, " columns");
      }
    }
    if (const char* mb = std::getenv("PLUSMC_CHUNK_BUDGET_MB")) {
      if (auto v = parse_clamped_int(mb, 0, 1 << 20)) {
        budget.bytes = static_cast<std::size_t>(*v) * 1024ull * 1024ull;
      } else {
        logging::warn("PLUSMC_CHUNK_BUDGET_MB='", mb,
                      "' is not an integer in 0..1048576; keeping "
                      "the default (no byte ceiling)");
      }
    }
    // Said out loud once at startup, so a host reading the log does not have to
    // work out which of the two ceilings is actually in force. A zero means the
    // side is unbounded, which is a fact worth printing rather than hiding.
    logging::info("chunk budget: ",
                  budget.columns == 0 ? "unbounded" : std::to_string(budget.columns), " columns, ",
                  budget.bytes == 0 ? std::string("no byte ceiling")
                                    : std::to_string(budget.bytes / (1024ull * 1024ull)) + " MB",
                  " (PLUSMC_CHUNK_BUDGET_COLUMNS / PLUSMC_CHUNK_BUDGET_MB)");
    return budget;
  };

  // Phase-4 milestone K's integration: --worldgen serves the built-in overworld
  // at its post-carver stage. It goes through the same in-memory store as the void world
  // (`MemoryWorld` with a generator: mutations live for the process), and its
  // spawn has no level.dat to come from, so the search above derives one from
  // the terrain itself.
  // Kept beside the world so the weather half can still reach level.dat after
  // the world is wrapped in the light recorder (which is not an AnvilWorld).
  std::shared_ptr<mc::world::AnvilWorld> anvil_world;
  if (cfg.worldgen_seed) {
    const bool nether = cfg.worldgen_dimension == "nether";
    const bool end = cfg.worldgen_dimension == "end";
    const mc::world::WgDensityTable& table =
        end ? mc::world::wg_end_table()
            : (nether ? mc::world::wg_nether_table() : mc::world::wg_overworld_table());
    const int min_section_y = (nether || end) ? 0 : mc::world::kOverworldMinSectionY;
    const int section_count = end ? 16 : (nether ? 8 : mc::world::kOverworldSectionCount);
    auto world = std::make_shared<mc::world::MemoryWorld>(
        min_section_y, section_count,
        std::make_shared<mc::world::WgOverworldGenerator>(*cfg.worldgen_seed, table),
        read_chunk_budget());
    const auto spawn =
        end ? std::optional<std::tuple<int, int, int>>{std::tuple<int, int, int>{100, 49, 0}}
            : (nether ? generated_nether_spawn(*world) : generated_world_spawn(*world));
    if (spawn) {
      settings.spawn_x = std::get<0>(*spawn);
      settings.spawn_y = std::get<1>(*spawn);
      settings.spawn_z = std::get<2>(*spawn);
    }
    settings.flat_world = false;
    settings.dimension =
        end ? "minecraft:the_end" : (nether ? "minecraft:nether" : "minecraft:overworld");
    settings.sea_level = end ? 0 : (nether ? 32 : 63);
    // Join Game carries the *hash* vanilla advertises, not the seed (see
    // SessionSettings::hashed_seed). Sending 0 told the client's biome manager
    // that the world had no seed while the terrain was built from this one.
    settings.hashed_seed = mc::world::obfuscate_seed(*cfg.worldgen_seed);
    settings.world = std::move(world);
    logging::info("worldgen: ", settings.dimension, "; seed ", *cfg.worldgen_seed, "; spawn ",
                  settings.spawn_x, ",", settings.spawn_y, ",", settings.spawn_z,
                  " (join game hashed seed ", settings.hashed_seed, ")");
  } else if (!cfg.world_dir.empty()) {
    // Phase-2 milestone A: serve an existing Anvil world. The overworld region
    // folder resolves per the 26.2 world layout; the spawn point comes from
    // level.dat when present (else the defaults below, which sit inside chunk
    // (0,0) so a fresh/trimmed world still shows terrain).
    auto anvil = mc::world::AnvilWorld::open_world(cfg.world_dir, mc::world::kOverworldMinSectionY,
                                                   mc::world::kOverworldSectionCount);
    if (!anvil) {
      logging::error("world directory ", cfg.world_dir.string(),
                     " is not readable (or cannot be created)");
      return 1;
    }
    settings.world = anvil;
    anvil_world = anvil;
    save_dir = anvil->world_root();
    logging::info("serving world ", cfg.world_dir.string(), " (regions under ",
                  anvil->region_dir().string(), ")");
    if (auto spawn = anvil->world_spawn()) {
      settings.spawn_x = std::get<0>(*spawn);
      settings.spawn_y = std::get<1>(*spawn);
      settings.spawn_z = std::get<2>(*spawn);
      logging::info("world spawn: ", settings.spawn_x, ", ", settings.spawn_y, ", ",
                    settings.spawn_z);
    } else {
      logging::warn("--world: no level.dat spawn found; using ", settings.spawn_x, ",",
                    settings.spawn_y, ",", settings.spawn_z);
    }
    // The seed that generated the file's terrain, hashed exactly as `--worldgen`
    // does (SessionSettings::hashed_seed): vanilla's Join Game and Respawn carry
    // `obfuscateSeed(level.getSeed())`, and the client's biome manager samples
    // its unloaded-chunk fallback through it. A file with no seed to read keeps
    // the default 0 rather than inventing one.
    if (auto seed = anvil->level_seed()) {
      settings.hashed_seed = mc::world::obfuscate_seed(*seed);
      logging::info("level seed: ", *seed, " (join game hashed seed ", settings.hashed_seed, ")");
    } else {
      logging::info("level.dat carries no seed; join game hashed seed stays 0");
    }
  } else {
    // No --world/--worldgen: create an in-memory mutable world so block
    // place/break work in the flat/void mode. The first two arguments are the
    // store's own defaults spelled out (the void column is the overworld's
    // shape of air) because the budget is the fourth parameter.
    settings.world = std::make_shared<mc::world::MemoryWorld>(mc::world::kOverworldMinSectionY,
                                                              mc::world::kOverworldSectionCount,
                                                              nullptr, read_chunk_budget());
  }
  // Resume the level's weather from level.dat, ramped levels included. A world
  // with no file, or one without the fields, stays clear; a file written before
  // the levels were persisted falls back to full strength for a storm.
  if (anvil_world) {
    if (const auto saved = anvil_world->level_weather();
        saved &&
        (saved->raining || saved->thundering || saved->rain_time > 0 || saved->thunder_time > 0 ||
         saved->rain_level > 0.0f || saved->thunder_level > 0.0f)) {
      mc::server::SavedWeather weather;
      weather.rain_time = saved->rain_time;
      weather.thunder_time = saved->thunder_time;
      weather.raining = saved->raining;
      weather.thundering = saved->thundering;
      weather.rain_level = saved->rain_level;
      weather.thunder_level = saved->thunder_level;
      mc::server::restore_weather(weather);
      logging::info("weather: resumed ", saved->raining ? "rain" : "clear",
                    saved->thundering ? " + thunder" : "", " (", saved->rain_time, " rain ticks, ",
                    saved->thunder_time, " thunder ticks, levels ", saved->rain_level, "/",
                    saved->thunder_level, ")");
    }
  }
  // Phase 4 milestone B: wrap the world in the light recorder. Every block write
  // the server makes goes through the world interface, so wrapping it here — once,
  // around either world — is what lets the light pass see *every* write (a
  // placement, a break, a fire spreading, a leaf decaying, a blast) without each
  // of those call sites having to remember to say so. See world/world_light.h for
  // what a write marks and world/light.h for the engine it feeds.
  settings.world = std::make_shared<mc::world::LightWorld>(settings.world);
  // Ahead-of-view chunk generation (world/chunk_generation.h): a small pool that
  // builds the columns a session is about to stream, so sessions send instead of
  // generating. Installed only for a world that has to *build* its columns — a
  // void world's absence is air, and an Anvil world's comes off disk — and
  // PLUSMC_GEN_THREADS is both the size knob and the A/B arm: 0 keeps the
  // engine's original inline path on the same binary, which is how the change is
  // measured (docs/PHASE5.md). The default is one thread fewer than the box has
  // cores, so the pool works beside the sim (and beside the sessions' own light
  // and encoding) rather than on top of it.
  if (cfg.worldgen_seed) {
    const unsigned cores = std::thread::hardware_concurrency();
    const int ceiling = cores > 2 ? std::min(4u, cores - 1) : 1;
    int gen_threads = ceiling;
    if (const char* text = std::getenv("PLUSMC_GEN_THREADS"))
      if (auto v = parse_clamped_int(text, 0, 64))
        gen_threads = *v;
    if (gen_threads > 0) {
      mc::world::ChunkGeneration::Options gen_options;
      gen_options.threads = gen_threads;
      settings.generation =
          std::make_shared<mc::world::ChunkGeneration>(*settings.world, gen_options);
      logging::info("chunk generation: ", gen_threads,
                    " thread(s) building columns ahead of the view "
                    "(PLUSMC_GEN_THREADS=0 selects the inline path)");
    }
  }
  // Advertised worldgen/biome order feeds the chunk encoder's biome lookups.
  // The registry id string depends on the clientconfig filename naming, so
  // select by content (the only listing that contains biome names).
  for (const auto& [rid, entries] : settings.client_config.registries) {
    bool has_plains = false, has_void = false;
    for (const auto& e : entries) {
      if (e == "minecraft:plains")
        has_plains = true;
      if (e == "minecraft:the_void")
        has_void = true;
    }
    if (has_plains && has_void) {
      settings.biome_registry = entries;
      // The encoder's name -> wire-id map is the same for every column and
      // every connection; build it once here rather than inside
      // `chunk_body_for` for each chunk sent (session.h's `biome_table`).
      settings.biome_table =
          std::make_shared<mc::world::NameListBiomeTable>(settings.biome_registry);
      break;
    }
  }

  mc::net::TcpServer server;
  mc::net::TcpServer::Options opts;
  opts.host = cfg.host;
  opts.port = cfg.port;
  // The socket's receive timeout is the "peer is gone" backstop, and the
  // keep-alive is what decides liveness in the play state: a keep-alive that is
  // due at 60 s can never be answered inside a 30 s socket timeout, so a server
  // with a tuned cadence would drop idle clients that were perfectly healthy.
  // Derive it from the cadence (+ a second of slack for a loaded box), floored
  // at vanilla's own 30 s so the default configuration is unchanged.
  opts.receive_timeout_ms =
      std::max(mc::net::Connection::kDefaultReceiveTimeoutMs,
               settings.keep_alive_ms + settings.keep_alive_timeout_ms + 1000);
  logging::info("socket read timeout: ", opts.receive_timeout_ms / 1000, "s (keep-alive ",
                settings.keep_alive_ms / 1000, "s + ", settings.keep_alive_timeout_ms / 1000,
                "s reply window)");
  opts.handler = [settings](mc::net::Connection& conn) {
    mc::server::serve_connection(conn, settings);
  };

  // The metrics *export* (Phase-5 item 8): one JSON object per reported window,
  // appended to --metrics-file so a host can tail or forward it. Opened before
  // the listener so a path the process cannot write fails the start rather than
  // the first interval (a host that asked for the export and silently got none
  // has the worst of both). The stderr line keeps its own contract: the file is
  // additive, and bench/soak.py still parses the line.
  std::ofstream metrics_out;
  if (!cfg.metrics_file.empty()) {
    metrics_out.open(cfg.metrics_file, std::ios::out | std::ios::app);
    if (!metrics_out) {
      logging::error("cannot open --metrics-file ", cfg.metrics_file.string(),
                     " for append (does its directory exist?)");
      return 1;
    }
    logging::info("metrics file: ", cfg.metrics_file.string(), " (one JSON object per window)");
  }

  // Autosave every 60s (600 ticks vanilla) — env-tunable for tests.
  int autosave_s = 60;
  if (const char* as = std::getenv("PLUSMC_AUTOSAVE_SECS"))
    if (auto v = parse_clamped_int(as, 0, 3600))
      autosave_s = static_cast<int>(*v);
  // The backup pass's own cadence (Phase-5 ops). Off unless asked for: a
  // snapshot is a directory copy, so a server that takes one every minute
  // without being told is a server that fills a disk. Env-tunable so a test can
  // watch a pass without waiting a minute (PLUSMC_BACKUP_SECS).
  int backup_interval_s = cfg.backup_interval_s;
  if (const char* bs = std::getenv("PLUSMC_BACKUP_SECS"))
    if (auto v = parse_clamped_int(bs, 0, 86400))
      backup_interval_s = static_cast<int>(*v);
  // Day/night cycle: advance the overworld clock at 20 TPS and sync clients
  // once a second. PLUSMC_DAY_CYCLE=0 freezes it (deterministic tests).
  bool day_cycle = true;
  if (const char* dc = std::getenv("PLUSMC_DAY_CYCLE"))
    day_cycle = std::string(dc) != "0" && !std::string(dc).empty();
  // Furnace/blast-furnace/smoker cook time is real: a 200-tick smelt takes ten
  // seconds at 20 TPS. PLUSMC_TICK_SCALE multiplies the ticks the loop advances
  // per real tick, so a test can watch a furnace finish quickly without a
  // second implementation of the tick (the same kind of test knob as
  // PLUSMC_CHUNKS_PER_TICK). It scales the clock too, so a scaled run is
  // self-consistent.
  int64_t tick_scale = 1;
  if (const char* ts = std::getenv("PLUSMC_TICK_SCALE"))
    if (auto v = parse_clamped_int(ts, 1, 1000))
      tick_scale = *v;
  // The `randomTickSpeed` gamerule (vanilla's default 3), which is how many
  // precipitation samples a chunk draws per tick. Vanilla's one-in-48 gate per
  // draw means a cauldron takes about an hour of in-game rain to fill, so this
  // is the knob a test uses to watch the weather actually do something — the
  // same kind of knob as PLUSMC_TICK_SCALE, without a second implementation of
  // the tick.
  if (const char* rts = std::getenv("PLUSMC_RANDOM_TICK_SPEED"))
    if (auto v = parse_clamped_int(rts, 0, 4096))
      mc::server::set_random_tick_speed(*v);
  // Natural spawning (`NaturalSpawner`, Phase 4 milestone C) is on by default —
  // the same floor vanilla's `SPAWN_MOBS` gamerule defaults to — and
  // PLUSMC_MOB_SPAWNING=0 turns the pass off entirely. A test that wants to
  // observe a spawn pass without the rest of the world's mob traffic turns it
  // off elsewhere; a test that measures a spawn pass turns it on.
  if (const char* ms = std::getenv("PLUSMC_MOB_SPAWNING"))
    mc::server::set_natural_spawning(std::string(ms) != "0" && !std::string(ms).empty());
  // The world spawn point the spawn pass keeps clear (it comes from level.dat
  // when the world has one, else the built-in default).
  mc::server::set_server_spawn(settings.spawn_x, settings.spawn_y, settings.spawn_z);
  mc::server::set_server_world(settings.world);
  // The plugin API's read-only accessors (docs/PLUGIN_API.md, slice 2). The
  // player half reads the session registry's snapshot (its own lock), the
  // world half the registered world (which is thread-safe for reads); neither
  // gives a plugin a live handle it could outlive. Registered before any
  // plugin loads, so `on_load`/`on_enable` can already query.
  mc::plugins::host().set_accessors(
      [](const std::string& name, mc::plugins::PlayerView* out) {
        mc::server::PlayerSnapshot snap;
        if (!mc::server::lookup_player_snapshot(name, &snap))
          return false;
        out->name = snap.name;
        out->x = snap.x;
        out->y = snap.y;
        out->z = snap.z;
        out->yaw = snap.yaw;
        out->pitch = snap.pitch;
        out->gamemode = snap.gamemode;
        out->health = snap.health;
        out->entity_id = snap.entity_id;
        out->vx = snap.vx;
        out->vy = snap.vy;
        out->vz = snap.vz;
        return true;
      },
      [world = settings.world](int32_t x, int32_t y, int32_t z, std::string* state,
                               std::string* biome) {
        if (!world)
          return false;
        // `load_chunk`, not the resident-only read: a plugin asking for a block
        // means the world's answer, and vanilla's `Level.getBlockState` loads
        // the chunk too. On a generated world this may build the column (the
        // store's own read answers air for terrain it has not materialised),
        // which is why the header says a position read belongs in a command or
        // an event, not in a per-tick loop over many positions.
        const auto chunk = world->load_chunk(x >> 4, z >> 4);
        static const mc::world::BlockStateKey kAir{"minecraft:air", ""};
        const auto& key = chunk ? chunk->block_state_at(x & 15, y, z & 15) : kAir;
        *state = key.name;
        if (!key.properties.empty())
          *state += "[" + key.properties + "]";
        if (biome != nullptr)
          *biome = world->biome_name_at(x, y, z);
        return true;
      });
  // The plugin API's write half (docs/PLUGIN_API.md, slice 3). It is the
  // engine's own `/setblock` path, not a column poke, so a plugin's write and a
  // player's write are indistinguishable to the clients and the tick.
  mc::plugins::host().set_writer([](int32_t x, int32_t y, int32_t z, const std::string& state) {
    return static_cast<int>(mc::server::plugin_set_block(x, y, z, state));
  });
  // Slice 4: the entity listing and the player inventory. The listing is a
  // snapshot taken under each registry's own lock (players, mobs, drops, orbs,
  // TNT, projectiles, potions and clouds); the inventory read copies the
  // player's published slot and the write is queued for that player's own
  // session, so a plugin never touches another thread's menu state.
  // Slice 5: the entity writes. Despawn and teleport run against the owning
  // registry here; a player's teleport and damage are queued for their own
  // session by the engine, so a plugin never touches another thread's state.
  mc::plugins::host().set_entity_writes(
      [](int32_t entity_id) {
        return static_cast<int>(mc::server::plugin_entity_despawn(entity_id));
      },
      [](int32_t entity_id, float amount) {
        return static_cast<int>(mc::server::plugin_entity_damage(entity_id, amount));
      },
      [](int32_t entity_id, double x, double y, double z) {
        return static_cast<int>(mc::server::plugin_entity_teleport(entity_id, x, y, z));
      });
  // The engine's snapshot shape to the host's; the listing and the by-id read
  // share the conversion so both see exactly the same fields.
  const auto to_entity_view = [](const mc::server::PluginEntity& entity) {
    mc::plugins::EntityView view;
    view.entity_id = entity.entity_id;
    view.type_id = entity.type_id;
    view.type = entity.type;
    view.name = entity.name;
    view.x = entity.x;
    view.y = entity.y;
    view.z = entity.z;
    view.health = entity.health;
    view.age = entity.age;
    view.vx = entity.vx;
    view.vy = entity.vy;
    view.vz = entity.vz;
    view.yaw = entity.yaw;
    view.pitch = entity.pitch;
    return view;
  };
  mc::plugins::host().set_entities([to_entity_view](std::vector<mc::plugins::EntityView>* out) {
    std::vector<mc::server::PluginEntity> entities;
    if (!mc::server::plugin_list_entities(&entities))
      return false;
    out->reserve(entities.size());
    for (const mc::server::PluginEntity& entity : entities)
      out->push_back(to_entity_view(entity));
    return true;
  });
  mc::plugins::host().set_entity_getter(
      [to_entity_view](int32_t entity_id, mc::plugins::EntityView* out) {
        mc::server::PluginEntity entity;
        const mc::server::EntityResult result = mc::server::plugin_get_entity(entity_id, &entity);
        if (result != mc::server::EntityResult::Ok)
          return static_cast<int>(result);
        *out = to_entity_view(entity);
        return static_cast<int>(mc::server::EntityResult::Ok);
      });
  // Slice 19: the by-name lookup. The engine resolves the first match in the
  // ABI's fixed order and the same snapshot conversion feeds the plugin.
  mc::plugins::host().set_entity_finder(
      [to_entity_view](const std::string& name, mc::plugins::EntityView* out) {
        mc::server::PluginEntity entity;
        const mc::server::EntityResult result = mc::server::plugin_find_entity(name, &entity);
        if (result != mc::server::EntityResult::Ok)
          return static_cast<int>(result);
        *out = to_entity_view(entity);
        return static_cast<int>(mc::server::EntityResult::Ok);
      });
  mc::plugins::host().set_inventory(
      [](const std::string& player, int slot, mc::plugins::ItemView* out) {
        mc::server::PluginItem item;
        const mc::server::InventoryResult result =
            mc::server::plugin_inventory_get(player, slot, &item);
        if (result != mc::server::InventoryResult::Ok)
          return static_cast<int>(result);
        out->item = item.item;
        out->count = item.count;
        out->damage = item.damage;
        out->max_stack_size = item.max_stack_size;
        out->name = std::move(item.name);
        return static_cast<int>(mc::server::InventoryResult::Ok);
      },
      [](const std::string& player, int slot, const std::string& item, int count) {
        return static_cast<int>(mc::server::plugin_inventory_set(player, slot, item, count));
      });
  // The population load sits between the world registration and the listener:
  // nothing has spawned yet, so the file's mobs and id counter can be adopted
  // whole. A file that cannot be read is not fatal — the world still serves,
  // starting with an empty population (the read already logged why).
  if (!save_dir.empty() && !mc::server::load_mob_population(save_dir))
    logging::warn("mobs: could not read the mob population; starting empty");
  // The advancement store is the player-scope twin of that: see session.h.
  if (!save_dir.empty() && !mc::server::load_advancement_state(save_dir))
    logging::warn("advancements: could not read the store; starting empty");
  // The other two stores the same order protects: a player's stat counters and
  // the server's scoreboard. Both are read before the listener for the same
  // reason — a join's `adopt_saved_stats` and the board's first command must
  // see what the last boot left.
  if (!save_dir.empty() && !mc::server::load_stats_state(save_dir))
    logging::warn("stats: could not read the store; starting empty");
  if (!save_dir.empty() && !mc::server::load_scoreboard_state(save_dir))
    logging::warn("scoreboard: could not read the board; starting empty");
  // Only now does the listener start. A connection thread can reach a join's
  // advancement adoption (and its mob population lookups) the instant the
  // socket is accepted, so a listener opened before these loads lets a fast
  // client join against a half-loaded process: `adopt_saved_advancements`
  // finds no row and the rejoining player's restored tree is silently missing
  // for the whole session. Whether it happens is a race, and under load it
  // happens often — the load is a gzip inflate plus an NBT parse, and the
  // client only has to reach its first join packet first. `load_*` documents
  // that it "must run before the listener starts"; this is that order. The
  // spawn point, the world and the tick env above are registered ahead of the
  // listener for the same reason: a session must never be served by a process
  // that has not finished becoming the world it advertises.
  // Plugins load before the listener opens, so no client can join a process
  // whose chat filter or command surface is not there yet. `on_enable` runs
  // here too; PMC_EVENT_SERVER_START waits until the listener is up.
  std::vector<fs::path> packaged_mods;
  std::string mods_error;
  if (!prepare_mods(cfg, &packaged_mods, &mods_error)) {
    logging::error(mods_error);
    return 1;
  }
  if (!cfg.plugins_dir.empty()) {
    std::string plugin_error;
    if (!mc::plugins::host().load_directory(cfg.plugins_dir.string(), &plugin_error)) {
      logging::error(plugin_error);
      return 1;
    }
  }
  for (const fs::path& root : packaged_mods) {
    std::string plugin_error;
    if (!mc::plugins::host().load_directory(root.string(), &plugin_error)) {
      logging::error(plugin_error);
      return 1;
    }
  }
  const bool any_plugins = !cfg.plugins_dir.empty() || !packaged_mods.empty();
  if (any_plugins)
    mc::plugins::host().enable_all();
  if (!cfg.wasm_plugins_dir.empty()) {
    std::string wasm_error;
    if (!mc::server::wasm_plugins().load_directory(cfg.wasm_plugins_dir, cfg.config_dir, settings,
                                                   &wasm_error)) {
      logging::error(wasm_error);
      return 1;
    }
  }
  if (!server.start(std::move(opts)))
    return 1;

  logging::info("listening on ", cfg.host, ":", server.port());
  if (any_plugins)
    mc::plugins::host().fire(PMC_EVENT_SERVER_START, nullptr, nullptr, {}, false);
  // ---- the fixed-timestep loop (Phase 5) --------------------------------
  // Vanilla's clock is 20 ticks a second, so this loop is a fixed-timestep
  // scheduler: wall time is turned into whole 50 ms ticks, and what the tick
  // work costs comes out of the *wait*, never out of the rate. Two properties
  // make that true, and both are measured (core/metrics.h, docs/PHASE5.md):
  //
  //   * The wait is `sleep_until(deadline)`, not `sleep_for(50 ms)`. Sleeping a
  //     flat 50 ms *after* the work makes the real period `50 ms + work`, which
  //     is why this loop used to run at ~19.70 TPS idle instead of 20.0, with the
  //     deficit repaid in catch-up bursts (the metric reporter caught it on its
  //     first run; docs/PHASE5.md has the before/after). An absolute deadline
  //     keeps the period at exactly 50 ms while the work fits inside it.
  //   * The tick count is the *measured* wall time since the last tick, not the
  //     assumption of one tick per iteration. A late loop therefore runs the
  //     ticks it missed — vanilla's own catch-up — bounded by `kMaxBacklogUs`:
  //     more than five ticks behind, the backlog is dropped and counted (the
  //     `resync` key in the metrics line) rather than carried forever. Dropping
  //     it is a choice, not a discovery: an unbounded backlog turns a slow
  //     machine into a death spiral, and a world clock that slips is closer to
  //     vanilla than one that tries to run a minute of backlog in a second.
  constexpr int64_t kMaxBacklogUs = 5 * mc::metrics::kTickUs;
  // How often an idle store is asked to give back what it can rebuild; see the
  // idle-release note in the loop. Ten seconds is a memory gesture's cadence —
  // long enough that a player reconnecting between two passes is never
  // interrupted by one, short enough that a host watching RSS sees it move.
  constexpr int kIdleReleaseSecs = 10;
  // ...and how small it has to get: the `keep` floor the release leaves behind.
  // Zero is legal ("give back everything rebuildable") and is what the store's
  // own test pins, but the default is small and *non-zero* on purpose: the
  // columns the release keeps are the most-recently-served ones — the LRU order
  // is what picks the victims — so a player who reconnects between two passes
  // finds the terrain it was last looking at still built. At ~460 KB a
  // full-height overworld column, 64 columns is ~29 MB, about 1.5% of the ~2 GB
  // plateau this release exists to collapse (docs/PHASE5.md §4), and a void-mode
  // column is a fraction of that.
  constexpr std::size_t kIdleReleaseFloorColumns = 64;
  std::size_t idle_floor_columns = kIdleReleaseFloorColumns;
  if (const char* floor_cols = std::getenv("PLUSMC_CHUNK_FLOOR_COLUMNS")) {
    if (auto v = parse_clamped_int(floor_cols, 0, 1 << 20)) {
      idle_floor_columns = static_cast<std::size_t>(*v);
    } else {
      logging::warn("PLUSMC_CHUNK_FLOOR_COLUMNS='", floor_cols,
                    "' is not an integer in 0..1048576; keeping the default ",
                    kIdleReleaseFloorColumns, " columns");
    }
  }
  logging::info("idle release: floor ", idle_floor_columns, " columns every ", kIdleReleaseSecs,
                "s with nobody connected (PLUSMC_CHUNK_FLOOR_COLUMNS)");
  // The other half of the same memory story, said out loud once for the same
  // reason the budget is: a host reading the log should not have to work out
  // whether the store's cell arrays are mappings the kernel gets back or blocks
  // the allocator keeps. Read through the same accessor the allocator itself
  // uses (the value is cached on first call, so both sides see one answer).
  logging::info("cell storage: buffers from ", mc::core::page_mapping_min_bytes() / 1024,
                " KiB up get a mapping of their own and are handed back whole when a "
                "column is released (PLUSMC_PAGE_ALLOC_MIN_KB, 0 = one page, above 16 = "
                "the shared heap)");
  // The other buffer a released column has to give back, and the one that
  // cannot be mapped on its own: a 2 KiB light array would spend a page to hold
  // half of one, so the pool packs several into a page and unmaps it when all
  // are free. The line says which side the build is on, because the two sides
  // answer the release differently and a host comparing two logs must not read
  // the pooled sentence off the heap arm.
  if (mc::core::LightSlabPool::enabled()) {
    logging::info("light storage: ", mc::core::LightSlabPool::kSlotBytes / 1024, " KiB arrays in ",
                  mc::core::page_bytes() / 1024, " KiB pages, ",
                  mc::core::LightSlabPool::slots_per_page(),
                  " per page, unmapped when every slot is free (PLUSMC_LIGHT_ALLOC)");
  } else {
    logging::info("light storage: ", mc::core::LightSlabPool::kSlotBytes / 1024,
                  " KiB arrays on the shared heap, freed to the allocator on release and "
                  "not handed back until it chooses (PLUSMC_LIGHT_ALLOC=0)");
  }
  const auto started = std::chrono::steady_clock::now();
  auto deadline = started;
  auto last_tick = started;
  int64_t tick_remainder_us = 0;
  uint64_t ticks_total = 0;
  mc::metrics::Counter metrics_window;
  double metrics_interval_s = cfg.metrics_interval_s > 0 ? cfg.metrics_interval_s : 0.0;
  if (const char* ms = std::getenv("PLUSMC_METRICS_SECS"))
    if (auto v = parse_clamped_int(ms, 0, 86400))
      metrics_interval_s = static_cast<double>(*v);
  // The export needs a cadence to write at, so --metrics-file implies the
  // one-second window when no interval was asked for (--help says so). An
  // explicit --metrics-interval wins; 0/off is indistinguishable from unset,
  // which is why the file's presence, not the interval's value, decides.
  if (!cfg.metrics_file.empty() && metrics_interval_s <= 0)
    metrics_interval_s = 1.0;
  double metrics_next_s = metrics_interval_s;
  // Seconds since the loop started, which is what the metrics line reports as
  // `uptime_s` (the world clock is a different, gamerule-driven thing).
  auto uptime_seconds = [&started]() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  };
  // One reported window, in both of its forms: the stderr line that the bench
  // harness parses, and — when --metrics-file was given — the same numbers as a
  // JSON object appended to it. Reused by the in-loop report and the
  // shutdown report so a run shorter than one interval still exports. A write
  // failure is a warning, not a stop: the server's job is serving, and a full
  // disk should not take it down.
  auto report_metrics = [&](const mc::metrics::Summary& summary) {
    const int players = mc::server::online_player_count();
    const uint64_t rss = mc::metrics::resident_set_bytes();
    // The store's occupancy rides the same window (src/core/metrics.h's
    // `WorldStore`): RSS says the process is 2 GB, this says whether that is a
    // streamed view set, a leak, or world state the store may not give back. The
    // reads are the store's own O(1) accounting, and a world with no column
    // cache (--world) answers zeros. `settings.world` is the light recorder,
    // which forwards to the store underneath.
    mc::metrics::WorldStore gauge;
    if (settings.world) {
      gauge.columns = settings.world->resident_columns();
      gauge.bytes = settings.world->resident_bytes();
      gauge.written_columns = settings.world->columns_with_writes();
      // Cumulative, unlike the three above: a budget that evicts makes the
      // resident gauge undercount a fill, and this is what a throughput reader
      // (`bench/soak.py`'s generation block) computes columns/s from.
      gauge.stored_columns = settings.world->columns_stored();
    }
    logging::info(mc::metrics::format_line(summary, players, rss, gauge));
    if (metrics_out.is_open()) {
      metrics_out << mc::metrics::format_json_line(summary, players, rss,
                                                   static_cast<int64_t>(std::time(nullptr)), gauge)
                  << '\n';
      metrics_out.flush();
      if (!metrics_out)
        logging::warn("metrics: could not append to ", cfg.metrics_file.string());
    }
  };
  auto last_save = started;
  auto last_backup = started;
  auto last_idle_release = started;
  // The weather rides level.dat, not the region files. Every flush writes it
  // first, so a crash loses at most one autosave window of storm — not the
  // whole storm since the last clean shutdown.
  auto persist_weather = [&anvil_world](bool announce) {
    if (!anvil_world)
      return;
    const mc::server::SavedWeather weather = mc::server::saved_weather();
    mc::world::AnvilWorld::LevelWeather saved;
    saved.rain_time = weather.rain_time;
    saved.thunder_time = weather.thunder_time;
    saved.raining = weather.raining;
    saved.thundering = weather.thundering;
    saved.rain_level = weather.rain_level;
    saved.thunder_level = weather.thunder_level;
    if (anvil_world->save_level_weather(saved)) {
      if (announce)
        logging::info("weather: saved to level.dat");
    } else {
      logging::warn("weather: could not be saved to level.dat");
    }
  };
  while (!g_stop_requested) {
    deadline += std::chrono::microseconds(mc::metrics::kTickUs);
    const auto before_wait = std::chrono::steady_clock::now();
    std::this_thread::sleep_until(deadline);
    const auto now = std::chrono::steady_clock::now();
    const auto sleep_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - before_wait).count();
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(now - last_tick).count();
    last_tick = now;
    tick_remainder_us += elapsed_us;
    bool dropped_backlog = false;
    if (tick_remainder_us > kMaxBacklogUs) {
      tick_remainder_us = 0;
      dropped_backlog = true;
    }
    const int64_t ticks = tick_remainder_us / mc::metrics::kTickUs;
    int64_t work_us = 0;
    if (ticks > 0) {
      tick_remainder_us -= ticks * mc::metrics::kTickUs;
      const auto work_start = std::chrono::steady_clock::now();
      if (day_cycle)
        mc::server::advance_world_time(ticks * tick_scale);
      mc::server::tick_world(ticks * tick_scale);
      mc::server::wasm_plugins().tick(static_cast<uint64_t>(ticks));
      mc::plugins::host().tick(static_cast<uint64_t>(ticks));
      work_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - work_start)
                    .count();
      ticks_total += static_cast<uint64_t>(ticks);
    }
    metrics_window.record(ticks, work_us, sleep_us, sleep_us + work_us, dropped_backlog);
    if (metrics_interval_s > 0) {
      const double uptime_s = uptime_seconds();
      if (uptime_s >= metrics_next_s) {
        metrics_next_s = uptime_s + metrics_interval_s;
        report_metrics(mc::metrics::summarise(metrics_window, ticks_total, uptime_s));
        mc::metrics::reset(metrics_window);
      }
    }
    if (autosave_s > 0 && settings.world) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_save).count() >= autosave_s) {
        last_save = now;
        persist_weather(/*announce=*/false);
        if (settings.world->save_all())
          logging::info("autosave: flushed");
        else
          logging::warn("autosave: flush failed (will retry)");
        // The population rides the same cadence as the chunks: an entity id a
        // crash loses is a reference a goal holds, so the two are kept fresh
        // together. Serializing the whole population is not free at scale,
        // which is why it shares the autosave interval and does not run per
        // tick.
        if (!save_dir.empty() && !mc::server::save_mob_population(save_dir))
          logging::warn("autosave: mob population save failed");
        // ...and the players' progress rides the same cadence.
        if (!save_dir.empty() && !mc::server::save_advancement_state(save_dir))
          logging::warn("autosave: advancement store save failed");
        if (!save_dir.empty() && !mc::server::save_stats_state(save_dir))
          logging::warn("autosave: stats store save failed");
        if (!save_dir.empty() && !mc::server::save_scoreboard_state(save_dir))
          logging::warn("autosave: scoreboard save failed");
      }
    }
    // The backup pass (Phase-5 ops, docs/PHASE5.md §6 item 8). A snapshot is
    // only worth taking off *quiescent* files, so every pass flushes first —
    // chunks, then the two stores — and copies what the flush just wrote. That
    // is the host's own recipe (`/save-all`, then copy the world) done in one
    // step, without stopping the server. Rotation rides along: create_backup
    // leaves at most --backup-keep snapshots behind.
    if (backup_interval_s > 0 && !save_dir.empty() && settings.world) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_backup).count() >=
          backup_interval_s) {
        last_backup = now;
        if (!settings.world->save_all())
          logging::warn("backup: world flush failed; the snapshot may be stale");
        if (!mc::server::save_mob_population(save_dir))
          logging::warn("backup: mob population save failed");
        if (!mc::server::save_advancement_state(save_dir))
          logging::warn("backup: advancement store save failed");
        if (!mc::server::save_stats_state(save_dir))
          logging::warn("backup: stats store save failed");
        if (!mc::server::save_scoreboard_state(save_dir))
          logging::warn("backup: scoreboard save failed");
        const mc::server::BackupResult backup =
            mc::server::create_backup(save_dir, cfg.backup_dir, cfg.backup_keep);
        if (!backup.ok) {
          logging::warn("backup: ", backup.error);
        } else {
          std::string rotated;
          if (backup.removed > 0)
            rotated = ", rotated " + std::to_string(backup.removed) + " out";
          logging::info("backup: wrote ", backup.path.string(), " (", backup.files, " files, ",
                        backup.bytes, " bytes", rotated, ")");
        }
      }
    }

    // The idle release (docs/PHASE5.md §4, "Per-view memory"). Eviction only
    // runs when a column is *stored*, and once a population disconnects nothing
    // stores one — the measured symptom was a 40-bot spread-256 view set still
    // holding ~2 GB ten seconds after the last bot left. With nobody connected
    // there is nothing watching what the store streamed, which is the one moment
    // a server can hand its rebuildable columns back; written ones are never
    // released (the map is MemoryWorld's only copy of them), so this gives back
    // the view set, not the world. The cadence is deliberately slow: it is a
    // memory gesture, not a tick cost.
    //
    // It is two gestures, not one, because on both platforms this server runs on
    // "freed" and "given back" are different things: dropping a column returns
    // whatever it holds from the shared heap to the allocator, which keeps the
    // free pages in its arenas — measured on the 40-bot cell before the cells
    // moved out of it, the store's own count went to zero while RSS did not move
    // at all. So the release is followed by `release_free_pages()` (the
    // platform's allocator gesture: pressure relief on macOS, `malloc_trim` on
    // glibc) and the log line reports both halves: what the store gave up and
    // what the OS actually got back. The RSS numbers are printed either side of
    // the gesture because that difference *is* the answer.
    //
    // The bulk of a column — its 24 section cell arrays, 16 KiB each — is no
    // longer the allocator's to hold: `core/page_alloc.h` gives those buffers
    // mappings of their own and munmaps them as the column is erased, so most of
    // the resident set this gesture used to read as immovable is already gone by
    // the time it runs, and `rss` on the line below moves by the whole column set
    // rather than by the gesture's leftovers. The call stays because the rest of
    // a column (light arrays, palettes, biome cells, the entities) is still the
    // heap's, and so is everything else the process streamed.
    if (settings.world && mc::server::online_player_count() == 0) {
      auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration_cast<std::chrono::seconds>(now - last_idle_release).count() >=
          kIdleReleaseSecs) {
        last_idle_release = now;
        // The two numbers a host cares about are sampled either side of the whole
        // gesture: the resident set (frames still occupied) and the footprint
        // (memory still charged to the process). They disagree after a big free —
        // macOS can leave RSS where it is while the footprint falls, because the
        // relieved pages are reusable rather than gone — so the line prints both
        // instead of picking one (src/core/metrics.h's `physical_footprint_bytes`).
        constexpr double kMb = 1024.0 * 1024.0;
        const uint64_t rss_before = mc::metrics::resident_set_bytes();
        const uint64_t footprint_before = mc::metrics::physical_footprint_bytes();
        // The pool's backlog goes first. Nobody is online, so the columns it
        // still has queued are prefetch for views that no longer exist; letting
        // the workers finish them would store a fresh ceiling right behind this
        // release, and the next pass would release the same 192 again — the
        // released count then reads as a multiple of what this store actually
        // gives back (docs/PHASE5.md §4). A worker already inside a column
        // finishes that one, and a later request queues the rest again.
        if (settings.generation)
          settings.generation->discard_pending();
        // The pool's booked pages either side of the gesture. Its *fall* is the
        // release's answer for the light arrays — the one buffer with no
        // per-array mapping to count from the store's side — and unlike the
        // process's RSS it cannot be moved by the allocator, by another thread's
        // fault-in, or by the relief gesture a pass or two later; the pool's
        // booked pages move only when a slot is freed or handed out. Read before
        // the store releases anything, printed after (docs/PHASE5.md §4).
        const std::size_t pool_pages_before = mc::core::LightSlabPool::mapped_pages();
        const std::size_t released = settings.world->release_idle_columns(idle_floor_columns);
        const mc::metrics::PageRelease back = mc::metrics::release_free_pages();
        if (released > 0 || back.allocator_bytes > 0 || back.rss_bytes > 0)
          logging::info("world store: released ", released, " idle columns; ",
                        settings.world->resident_columns(), " resident (",
                        settings.world->resident_bytes() / (1024ull * 1024ull), " MB), ",
                        settings.world->columns_with_writes(), " carrying writes; rss ",
                        static_cast<double>(rss_before) / kMb, " -> ",
                        static_cast<double>(mc::metrics::resident_set_bytes()) / kMb,
                        " MB, footprint ", static_cast<double>(footprint_before) / kMb, " -> ",
                        static_cast<double>(mc::metrics::physical_footprint_bytes()) / kMb,
                        " MB, allocator handed back ",
                        static_cast<double>(back.allocator_bytes) / kMb, " MB");
        // The light arrays are the one buffer with no per-array mapping to count
        // from the store's side, so the pool says what it is still holding: the
        // pages whose two slots were not both released. A line of its own keeps
        // the release line's shape stable for the records that quote it.
        if (mc::core::LightSlabPool::enabled())
          logging::info(
              "light pool: ", mc::core::LightSlabPool::mapped_bytes() / (1024ull * 1024ull),
              " MB in ", mc::core::LightSlabPool::mapped_pages(), " pages (",
              mc::core::LightSlabPool::slots_per_page(), " arrays per page; ", pool_pages_before,
              " before the release)");
      }
    }
  }

  // The window in flight is reported on the way out so a run shorter than one
  // interval still says what it cost (this is what a smoke test and the bench
  // harness read a two-second run from). A window with no ticks in it is not
  // reported: the loop can stop between an interval report and the next tick,
  // and appending an empty tail would leave the last record at `tps=0.00` for a
  // consumer that reads the last line (the schema's tick-rate floor is positive).
  if (metrics_interval_s > 0 && metrics_window.ticks > 0) {
    report_metrics(mc::metrics::summarise(metrics_window, ticks_total, uptime_seconds()));
  }
  // Plugins see the stop before anything is disabled: a plugin that still
  // wants to answer a player or flush its own state may. Unload is reverse
  // load order, so a plugin loaded after another is torn down first.
  mc::plugins::host().fire(PMC_EVENT_SERVER_STOP, nullptr, nullptr, {}, false);
  mc::plugins::host().unload_all();
  mc::server::wasm_plugins().unload_all();
  logging::info("shutting down");
  // The write path's own number, before the world is saved: how much of the
  // streaming was shared between connections rather than rebuilt per viewer
  // (docs/WRITE_PATH.md). Silent when nothing was ever sent.
  // The generation pool's own accounting, before the world is saved: how many
  // columns it built ahead of the view and how many asks were deduplicated or
  // dropped (world/chunk_generation.h). Silent when no pool was installed, which
  // is a statement in itself — the inline path has no line to print.
  if (settings.generation)
    logging::info(settings.generation->report());
  // What generation *cost*, not what the store kept: a column lost to a
  // publish race was still built (world/world_interface.h). Silent for a world
  // that never generated anything.
  if (const uint64_t builds = settings.world->generated_columns(); builds > 0)
    logging::info("column builds: ", builds, " generated (stored, evicted or lost to a race)");
  // ...and the biome memo's, which is what the generation pool's work is made
  // of: the hit rate is the only place the memo's own win is visible from
  // outside a profile (world/worldgen_biome.cpp).
  if (const std::string biome = mc::world::wg_biome_memo_report(); !biome.empty())
    logging::info(biome);
  if (const std::string share = mc::server::chunk_share_report(); !share.empty())
    logging::info(share);
  // ...and the natural spawn pass's (world/mob_spawn.h): the mobs it created
  // and the candidate square the store answered for. A soak's spawn rate is
  // this line divided by its window, and the refused count is the stored-column
  // predicate's own statement (docs/PHASE5.md's disk-world A/B).
  if (const std::string spawns = mc::server::natural_spawn_report(); !spawns.empty())
    logging::info(spawns);
  // ...and the entity relays': payloads built against client writes, which is
  // what the tracking gate changes and what a busier box cannot move.
  if (const std::string mobs = mc::server::mob_tracking_report(); !mobs.empty())
    logging::info(mobs);
  if (const std::string items = mc::server::item_tracking_report(); !items.empty())
    logging::info(items);
  if (const std::string orbs = mc::server::orb_tracking_report(); !orbs.empty())
    logging::info(orbs);
  if (const std::string tnt = mc::server::tnt_tracking_report(); !tnt.empty())
    logging::info(tnt);
  if (const std::string projectiles = mc::server::projectile_tracking_report();
      !projectiles.empty())
    logging::info(projectiles);
  if (const std::string potions = mc::server::potion_tracking_report(); !potions.empty())
    logging::info(potions);
  if (const std::string clouds = mc::server::cloud_tracking_report(); !clouds.empty())
    logging::info(clouds);
  persist_weather(/*announce=*/true);
  if (settings.world) {
    if (settings.world->save_all())
      logging::info("shutdown: world saved");
    else
      logging::warn("shutdown: world save failed");
  }
  if (!save_dir.empty()) {
    if (mc::server::save_mob_population(save_dir))
      logging::info("shutdown: mob population saved");
    else
      logging::warn("shutdown: mob population save failed");
    if (mc::server::save_advancement_state(save_dir))
      logging::info("shutdown: advancement store saved");
    else
      logging::warn("shutdown: advancement store save failed");
    if (mc::server::save_stats_state(save_dir))
      logging::info("shutdown: stats store saved");
    else
      logging::warn("shutdown: stats store save failed");
    if (mc::server::save_scoreboard_state(save_dir))
      logging::info("shutdown: scoreboard saved");
    else
      logging::warn("shutdown: scoreboard save failed");
  }
  server.shutdown();
  logging::info("bye");
  return 0;
}
