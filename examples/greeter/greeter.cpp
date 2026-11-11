// The plugin API's example: a chat filter, the player lifecycle events, the
// commands below and a heartbeat.
//
// It is small on purpose — it is the template a plugin author copies. It uses
// only `pluginapi/plugin.h`, links nothing from the engine, and is built as a
// shared module (CMakeLists.txt's `plusmc-plugin-greeter`). `tests/plugin_test.py`
// drives it over the real wire protocol.
#include "pluginapi/plugin.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Greeter {
  const pmc_host_api* api = nullptr;
  uint64_t heartbeat = 0;
  // Read before the listeners register; `/config` reads and rewrites the live
  // values through the appended config writers.
  std::string greeting;
  uint32_t heartbeat_ticks = 20;
  bool debug = false;
  uint64_t async_task = 0;
  // Written by `/blockguard` on one session thread and read by the block events
  // on whichever session thread owns the placer or breaker.
  std::atomic<bool> deny_place{false};
  std::atomic<bool> deny_break{false};
  std::atomic<bool> deny_item_use{false};
  // The most recent PMC_EVENT_ENTITY_DEATH, for `/deaths`. The world tick and
  // session threads both resolve deaths, so it is behind its own lock.
  std::mutex death_mutex;
  std::string last_death;
};

void log_line(const Greeter& greeter, int level, const std::string& text) {
  if (greeter.api != nullptr && greeter.api->log != nullptr)
    greeter.api->log(greeter.api->host, level, text.c_str());
}

// PMC_EVENT_PLAYER_CHAT: answer the sender privately and cancel the broadcast.
// Cancelling is the strong form of a chat plugin; a filter that only watches
// would leave `event->cancel` alone.
void on_chat(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* player = event->player != nullptr ? event->player : "?";
  const char* message = event->message != nullptr ? event->message : "";
  log_line(greeter, PMC_LOG_INFO, std::string("saw chat from ") + player + ": " + message);
  if (event->reply != nullptr && greeter.api->reply != nullptr) {
    const std::string reply = std::string("(greeter) ") + player + " said: " + message;
    greeter.api->reply(greeter.api->host, event->reply, reply.c_str());
  }
  event->cancel = 1;
}

// A second chat listener that only observes. The host runs every listener even
// when an earlier one cancels, so this line still appears for a chat the first
// handler above dropped from the broadcast; `tests/plugin_test.py` reads it
// from the log to pin that rule.
void on_chat_audit(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* player = event->player != nullptr ? event->player : "?";
  const char* message = event->message != nullptr ? event->message : "";
  log_line(greeter, PMC_LOG_INFO, std::string("audit saw chat from ") + player + ": " + message);
}

// PMC_EVENT_PLAYER_JOIN: the player is in play and can be answered. The event
// carries a reply handle, so the welcome is a normal feedback line; the lookup
// also proves the registry entry is already live before the event is delivered.
void on_join(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* player = event->player != nullptr ? event->player : "?";
  pmc_player snapshot{};
  snapshot.struct_size = sizeof(snapshot);
  const bool live = greeter.api->player_get != nullptr &&
                    greeter.api->player_get(greeter.api->host, player, &snapshot) == 0;
  const std::string state = live ? " (player_get: live)" : " (player_get: absent)";
  log_line(greeter, PMC_LOG_INFO, std::string("saw join: ") + player + state);
  if (event->reply != nullptr && greeter.api->reply != nullptr) {
    const std::string welcome = greeter.greeting.empty()
                                    ? std::string("(greeter) welcome, ") + player + "!"
                                    : greeter.greeting;
    greeter.api->reply(greeter.api->host, event->reply, welcome.c_str());
  }
}

// PMC_EVENT_PLAYER_LEAVE: the registry entry is already gone, so the lookup is
// expected to fail; the name and whatever the plugin saved are what remain.
void on_leave(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* player = event->player != nullptr ? event->player : "?";
  pmc_player snapshot{};
  snapshot.struct_size = sizeof(snapshot);
  const bool gone = greeter.api->player_get == nullptr ||
                    greeter.api->player_get(greeter.api->host, player, &snapshot) != 0;
  const std::string state = gone ? " (player_get: gone)" : " (player_get: live)";
  log_line(greeter, PMC_LOG_INFO, std::string("saw leave: ") + player + state);
}

// PMC_EVENT_BLOCK_PLACE / PMC_EVENT_BLOCK_BREAK: fired on the session thread
// before the world write. `/blockguard` can deny either kind; the event is then
// cancelled, the client is sent the existing state, and the reply handle answers
// the player. A watched event is still logged.
void on_block(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const bool place = event->type == PMC_EVENT_BLOCK_PLACE;
  const bool denied = place ? greeter.deny_place.load() : greeter.deny_break.load();
  const std::string action = place ? "place" : "break";
  const std::string block = event->block != nullptr ? event->block : "?";
  const std::string previous = event->previous != nullptr ? event->previous : "?";
  const std::string line = "block " + action + " at (" + std::to_string(event->block_x) + ", " +
                           std::to_string(event->block_y) + ", " + std::to_string(event->block_z) +
                           "): " + block + " (was " + previous + ")" + (denied ? " [denied]" : "");
  log_line(greeter, PMC_LOG_INFO, line);
  if (denied) {
    event->cancel = 1;
    if (event->reply != nullptr && greeter.api->reply != nullptr) {
      const std::string reply = "(greeter) block " + action + " denied";
      greeter.api->reply(greeter.api->host, event->reply, reply.c_str());
    }
  }
}

// PMC_EVENT_ITEM_USE_ON: a bucket or flint-and-steel use on a block. The
// clicked cell's state rides in the block fields and the item in `item`; a
// veto skips the fluid, TNT, fire, wear and stat. `/itemguard` sets the policy.
void on_item_use(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const bool denied = greeter.deny_item_use.load();
  const char* item = event->item != nullptr ? event->item : "?";
  const char* block = event->block != nullptr ? event->block : "?";
  const std::string line = "item use at (" + std::to_string(event->block_x) + ", " +
                           std::to_string(event->block_y) + ", " + std::to_string(event->block_z) +
                           ") face " + std::to_string(event->face) + ": " + item + " on " + block +
                           (denied ? " [denied]" : "");
  log_line(greeter, PMC_LOG_INFO, line);
  if (denied) {
    event->cancel = 1;
    if (event->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, event->reply, "(greeter) item use denied");
  }
}

// PMC_EVENT_ENTITY_DEATH: a mob died and its credit, loot and orbs are already
// resolved. Not cancelable. `/deaths` reads back the last one, which is how the
// wire test observes this event without scraping the server log.
void on_entity_death(void* data, pmc_event* event) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* type = event->entity_type != nullptr ? event->entity_type : "?";
  std::string line = std::string("death: ") + type + " #" + std::to_string(event->entity_id) +
                     " at " + std::to_string(static_cast<int>(std::floor(event->entity_x))) + ", " +
                     std::to_string(static_cast<int>(std::floor(event->entity_y))) + ", " +
                     std::to_string(static_cast<int>(std::floor(event->entity_z)));
  if (event->killer_name != nullptr)
    line += std::string(" by ") + event->killer_name;
  log_line(greeter, PMC_LOG_INFO, line);
  std::lock_guard<std::mutex> lock(greeter.death_mutex);
  greeter.last_death = std::move(line);
}

// /deaths — the last death the plugin saw, or a note that none has happened.
void on_deaths_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  std::string line;
  {
    std::lock_guard<std::mutex> lock(greeter.death_mutex);
    line = greeter.last_death.empty() ? "(deaths) no death seen yet"
                                      : "(deaths) " + greeter.last_death;
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line.c_str());
}

// /config <key> [value] — the flat config file. One argument prints the typed
// live value; two write one key and persist the whole file. A value that is
// exactly `true`/`false` is a boolean, a full base-10 integer is an integer,
// and anything else is a string, so the command needs no type argument.
void on_config_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (command->argc < 2 || command->argc > 3) {
    reply("(config) usage: /config <key> [value]");
    return;
  }
  const std::string key = command->argv[1];
  if (command->argc == 2) {
    if (greeter.api->config_string != nullptr) {
      const char* text = greeter.api->config_string(greeter.api->host, key.c_str());
      if (text != nullptr) {
        reply("(config) " + key + " = \"" + text + "\"");
        return;
      }
    }
    if (greeter.api->config_int != nullptr) {
      const int64_t value = greeter.api->config_int(greeter.api->host, key.c_str(),
                                                    std::numeric_limits<int64_t>::min());
      if (value != std::numeric_limits<int64_t>::min()) {
        reply("(config) " + key + " = " + std::to_string(value));
        return;
      }
    }
    if (greeter.api->config_bool != nullptr) {
      const int value = greeter.api->config_bool(greeter.api->host, key.c_str(), -1);
      if (value == 0 || value == 1) {
        reply("(config) " + key + " = " + (value != 0 ? "true" : "false"));
        return;
      }
    }
    reply("(config) " + key + " is unset");
    return;
  }

  const std::string value = command->argv[2];
  int code = PMC_CONFIG_INVALID;
  if (value == "true" || value == "false") {
    if (greeter.api->config_set_bool == nullptr) {
      reply("(config) this host cannot write its config");
      return;
    }
    code = greeter.api->config_set_bool(greeter.api->host, key.c_str(), value == "true" ? 1 : 0);
  } else {
    char* end = nullptr;
    errno = 0;
    const long long number = std::strtoll(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0' && errno != ERANGE) {
      if (greeter.api->config_set_int == nullptr) {
        reply("(config) this host cannot write its config");
        return;
      }
      code =
          greeter.api->config_set_int(greeter.api->host, key.c_str(), static_cast<int64_t>(number));
    } else {
      if (greeter.api->config_set_string == nullptr) {
        reply("(config) this host cannot write its config");
        return;
      }
      code = greeter.api->config_set_string(greeter.api->host, key.c_str(), value.c_str());
    }
  }
  if (code == PMC_CONFIG_OK)
    reply("(config) set " + key);
  else
    reply("(config) refused " + key + " (code " + std::to_string(code) + ")");
}

// /worldclock — the world clock and weather accessors. Reading both in one
// command is the small status pair a server plugin wants; the setters below
// prove the write half.
void on_worldclock_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->world_time == nullptr || greeter.api->world_weather == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(worldclock) this host has no world clock");
    return;
  }
  const int64_t ticks = greeter.api->world_time(greeter.api->host);
  const int weather = greeter.api->world_weather(greeter.api->host);
  static const char* kWeather[] = {"clear", "rain", "thunder"};
  const char* mode = weather >= 0 && weather <= 2 ? kWeather[weather] : "unknown";
  char line[192];
  std::snprintf(line, sizeof(line), "(worldclock) time %lld, day %lld, weather %s",
                static_cast<long long>(ticks), static_cast<long long>(ticks / 24000), mode);
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /setworldtime <ticks> — `world_set_time` is `/time set`'s path: the clock
// moves and every player gets the SetTime update in the same call.
void on_setworldtime_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto usage = [&] {
    if (command->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(setworldtime) usage: /setworldtime <ticks>");
  };
  if (greeter.api->world_set_time == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(setworldtime) this host has no world clock");
    return;
  }
  if (command->argc != 2) {
    usage();
    return;
  }
  char* end = nullptr;
  errno = 0;
  const long long ticks = std::strtoll(command->argv[1], &end, 10);
  if (end == command->argv[1] || *end != '\0' || errno == ERANGE || ticks < 0) {
    usage();
    return;
  }
  const int code = greeter.api->world_set_time(greeter.api->host, ticks);
  char line[128];
  if (code == PMC_WORLD_OK)
    std::snprintf(line, sizeof(line), "(setworldtime) time %lld", ticks);
  else
    std::snprintf(line, sizeof(line), "(setworldtime) refused (code %d)", code);
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /setweather <clear|rain|thunder> [ticks] — `world_set_weather` is
// `/weather`'s own path, including the mode's default duration when none is
// given.
void on_setweather_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto usage = [&] {
    if (command->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(setweather) usage: /setweather <clear|rain|thunder> [ticks]");
  };
  if (greeter.api->world_set_weather == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(setweather) this host has no weather");
    return;
  }
  if (command->argc < 2 || command->argc > 3) {
    usage();
    return;
  }
  const std::string mode = command->argv[1];
  int weather = PMC_WEATHER_CLEAR;
  if (mode == "clear")
    weather = PMC_WEATHER_CLEAR;
  else if (mode == "rain")
    weather = PMC_WEATHER_RAIN;
  else if (mode == "thunder")
    weather = PMC_WEATHER_THUNDER;
  else {
    usage();
    return;
  }
  int32_t duration = 0;
  if (command->argc == 3) {
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(command->argv[2], &end, 10);
    if (end == command->argv[2] || *end != '\0' || errno == ERANGE || parsed < 0 ||
        parsed > 2147483647LL) {
      usage();
      return;
    }
    duration = static_cast<int32_t>(parsed);
  }
  const int code = greeter.api->world_set_weather(greeter.api->host, weather, duration);
  char line[128];
  if (code == PMC_WORLD_OK) {
    if (duration > 0)
      std::snprintf(line, sizeof(line), "(setweather) weather %s for %d ticks", mode.c_str(),
                    duration);
    else
      std::snprintf(line, sizeof(line), "(setweather) weather %s", mode.c_str());
  } else {
    std::snprintf(line, sizeof(line), "(setweather) refused (code %d)", code);
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// The message text after the command name and its first argument: a plugin
// command receives tokens, so the spaces a player typed have to be put back.
std::string join_from(const pmc_command* command, int first) {
  std::string text;
  for (int i = first; i < command->argc; ++i) {
    if (!text.empty())
      text += ' ';
    text += command->argv[i];
  }
  return text;
}

// /dm <player> <message...> — `player_message` reaches a player by name through
// the same SystemChat path the engine's own chat uses.
void on_dm_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->player_message == nullptr) {
    reply("(dm) this host cannot message players");
    return;
  }
  if (command->argc < 3) {
    reply("(dm) usage: /dm <player> <message>");
    return;
  }
  const std::string text = join_from(command, 2);
  const int code = greeter.api->player_message(greeter.api->host, command->argv[1], text.c_str());
  if (code == PMC_MESSAGE_OK)
    reply(std::string("(dm) sent to ") + command->argv[1]);
  else if (code == PMC_MESSAGE_NO_PLAYER)
    reply(std::string("(dm) no player named ") + command->argv[1]);
  else if (code == PMC_MESSAGE_INVALID)
    reply("(dm) the message is empty or too long");
  else
    reply("(dm) refused (code " + std::to_string(code) + ")");
}

// /broadcast <message...> — the same line to everyone in play; the answer is
// how many players it reached.
void on_broadcast_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->broadcast == nullptr) {
    reply("(broadcast) this host cannot message players");
    return;
  }
  if (command->argc < 2) {
    reply("(broadcast) usage: /broadcast <message>");
    return;
  }
  const std::string text = join_from(command, 1);
  const int reached = greeter.api->broadcast(greeter.api->host, text.c_str());
  if (reached == PMC_MESSAGE_INVALID)
    reply("(broadcast) the message is empty or too long");
  else
    reply("(broadcast) sent to " + std::to_string(reached) + " player(s)");
}

// /online — the live player count, the read a hub or minigame plugin gates on.
void on_online_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (command->reply == nullptr)
    return;
  if (greeter.api->player_count == nullptr) {
    greeter.api->reply(greeter.api->host, command->reply,
                       "(online) this host cannot count players");
    return;
  }
  const int count = greeter.api->player_count(greeter.api->host);
  char line[96];
  if (count < 0)
    std::snprintf(line, sizeof(line), "(online) unavailable");
  else
    std::snprintf(line, sizeof(line), "(online) %d player(s)", count);
  greeter.api->reply(greeter.api->host, command->reply, line);
}

// A decimal argument for the player-state commands, or false when the token is
// not a finite number the whole way through.
bool parse_float_arg(const char* text, float* out) {
  if (text == nullptr || *text == '\0')
    return false;
  char* end = nullptr;
  errno = 0;
  const float value = std::strtof(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

// The four game-mode spellings the engine's own token parser accepts.
int gamemode_token(const char* text) {
  const std::string mode = text != nullptr ? text : "";
  if (mode == "0" || mode == "s" || mode == "survival")
    return 0;
  if (mode == "1" || mode == "c" || mode == "creative")
    return 1;
  if (mode == "2" || mode == "a" || mode == "adventure")
    return 2;
  if (mode == "3" || mode == "sp" || mode == "spectator")
    return 3;
  return -1;
}

// /setgm <player> <mode> — the queued game-mode write. The target's own session
// applies it through the same path `/gamemode` uses, so the client sees the
// GameEvent, the abilities packet and the tab-list update.
void on_setgm_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->player_set_gamemode == nullptr) {
    reply("(setgm) this host cannot write player state");
    return;
  }
  if (command->argc != 3) {
    reply("(setgm) usage: /setgm <player> <survival|creative|adventure|spectator>");
    return;
  }
  const int mode = gamemode_token(command->argv[2]);
  if (mode < 0) {
    reply("(setgm) unknown game mode");
    return;
  }
  const int code = greeter.api->player_set_gamemode(greeter.api->host, command->argv[1], mode);
  if (code == PMC_PLAYER_OK)
    reply(std::string("(setgm) queued ") + command->argv[2] + " for " + command->argv[1]);
  else if (code == PMC_PLAYER_NO_PLAYER)
    reply(std::string("(setgm) no player named ") + command->argv[1]);
  else
    reply("(setgm) refused (code " + std::to_string(code) + ")");
}

// /heal <player> [health] — health 0..20, defaulting to a full bar. The write is
// queued; the target's own set_health packet is the proof it applied.
void on_heal_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->player_set_health == nullptr) {
    reply("(heal) this host cannot write player state");
    return;
  }
  if (command->argc < 2 || command->argc > 3) {
    reply("(heal) usage: /heal <player> [health 0..20]");
    return;
  }
  float health = 20.0f;
  if (command->argc == 3 && !parse_float_arg(command->argv[2], &health)) {
    reply("(heal) usage: /heal <player> [health 0..20]");
    return;
  }
  const int code = greeter.api->player_set_health(greeter.api->host, command->argv[1], health);
  char line[160];
  if (code == PMC_PLAYER_OK) {
    std::snprintf(line, sizeof(line), "(heal) queued health %.2f for %s", health, command->argv[1]);
    reply(line);
  } else if (code == PMC_PLAYER_NO_PLAYER) {
    reply(std::string("(heal) no player named ") + command->argv[1]);
  } else {
    reply("(heal) refused (code " + std::to_string(code) + ")");
  }
}

// /feed <player> [food] [saturation] — the food level defaults to 20 and the
// saturation to the same number, which the engine clamps to the food level.
void on_feed_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->player_set_food == nullptr) {
    reply("(feed) this host cannot write player state");
    return;
  }
  if (command->argc < 2 || command->argc > 4) {
    reply("(feed) usage: /feed <player> [food 0..20] [saturation]");
    return;
  }
  int food = 20;
  float saturation = 20.0f;
  if (command->argc >= 3) {
    char* end = nullptr;
    errno = 0;
    const long parsed = command->argv[2] != nullptr ? std::strtol(command->argv[2], &end, 10) : 0;
    if (end == command->argv[2] || *end != '\0' || errno == ERANGE || parsed < 0 || parsed > 20) {
      reply("(feed) usage: /feed <player> [food 0..20] [saturation]");
      return;
    }
    food = static_cast<int>(parsed);
    saturation = static_cast<float>(food);
  }
  if (command->argc == 4 && !parse_float_arg(command->argv[3], &saturation)) {
    reply("(feed) usage: /feed <player> [food 0..20] [saturation]");
    return;
  }
  const int code =
      greeter.api->player_set_food(greeter.api->host, command->argv[1], food, saturation);
  char line[160];
  if (code == PMC_PLAYER_OK) {
    std::snprintf(line, sizeof(line), "(feed) queued food %d saturation %.2f for %s", food,
                  saturation, command->argv[1]);
    reply(line);
  } else if (code == PMC_PLAYER_NO_PLAYER) {
    reply(std::string("(feed) no player named ") + command->argv[1]);
  } else {
    reply("(feed) refused (code " + std::to_string(code) + ")");
  }
}

// /find <name> — the by-name entity lookup. Players resolve by account name,
// mobs by display or registry name, and the rest by the names the listing
// reports; the first match in the ABI's fixed order wins.
void on_find_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto reply = [&](const std::string& line) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  };
  if (greeter.api->entity_find == nullptr) {
    reply("(find) this host cannot look entities up");
    return;
  }
  if (command->argc != 2) {
    reply("(find) usage: /find <name>");
    return;
  }
  pmc_entity entity{};
  entity.struct_size = sizeof(entity);
  const int code = greeter.api->entity_find(greeter.api->host, command->argv[1], &entity);
  if (code == PMC_ENTITY_OK) {
    char line[256];
    std::snprintf(line, sizeof(line), "(find) %s: #%d %s at %.1f, %.1f, %.1f", command->argv[1],
                  entity.entity_id, entity.type, entity.x, entity.y, entity.z);
    reply(line);
  } else if (code == PMC_ENTITY_NOT_FOUND) {
    reply(std::string("(find) no entity named ") + command->argv[1]);
  } else {
    reply("(find) refused (code " + std::to_string(code) + ")");
  }
}

// /itemguard <allow|deny> — the policy `on_item_use` reads.
void on_itemguard_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto usage = [&] {
    if (command->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(itemguard) usage: /itemguard <allow|deny>");
  };
  if (command->argc != 2) {
    usage();
    return;
  }
  const std::string mode = command->argv[1];
  if (mode != "allow" && mode != "deny") {
    usage();
    return;
  }
  greeter.deny_item_use.store(mode == "deny");
  if (command->reply != nullptr && greeter.api->reply != nullptr) {
    const std::string line =
        std::string("(itemguard) use=") + (greeter.deny_item_use.load() ? "deny" : "allow");
    greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  }
}

// /blockguard <place|break> <allow|deny> — the policy `on_block` reads.
void on_blockguard_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  auto usage = [&] {
    if (command->reply != nullptr && greeter.api->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(blockguard) usage: /blockguard <place|break> <allow|deny>");
  };
  if (command->argc != 3) {
    usage();
    return;
  }
  const std::string kind = command->argv[1];
  const std::string mode = command->argv[2];
  if ((kind != "place" && kind != "break") || (mode != "allow" && mode != "deny")) {
    usage();
    return;
  }
  if (kind == "place")
    greeter.deny_place.store(mode == "deny");
  else
    greeter.deny_break.store(mode == "deny");
  const std::string line = std::string("(blockguard) place=") +
                           (greeter.deny_place.load() ? "deny" : "allow") +
                           " break=" + (greeter.deny_break.load() ? "deny" : "allow");
  if (command->reply != nullptr && greeter.api->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line.c_str());
}

// /greet [name] — registered by bare name; the server only offers plugins a
// line no built-in command accepted, so a built-in is never shadowed.
void on_greet_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  const std::string who =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "?");
  const std::string reply = "Hello, " + who + "! (from the greeter example plugin)";
  if (command->reply != nullptr && greeter.api->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, reply.c_str());
}

// /whereami [player] — the read-accessor example: a player snapshot (including
// the observed velocity) and two world queries, all copied answers rather than
// live handles. `/whereami` reports the sender; naming someone else reports
// them, which is also how the "unknown player" path is reachable.
void on_whereami_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  const char* target =
      command->argc > 1 ? command->argv[1] : (command->player != nullptr ? command->player : "");
  if (greeter.api->player_get == nullptr || greeter.api->world_block == nullptr ||
      greeter.api->world_biome == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(whereami) this host has no accessors");
    return;
  }
  pmc_player player{};
  player.struct_size = sizeof(pmc_player);
  if (greeter.api->player_get(greeter.api->host, target, &player) != 0) {
    const std::string reply = std::string("(whereami) no player named '") + target + "'";
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, reply.c_str());
    return;
  }
  // The blocks at and under the feet, and the biome there. A host without a
  // world answers -1 and the line says so rather than inventing a position's
  // content. "Standing on" is one cell down because a player's feet are the
  // air above the ground they stand on.
  char feet[128] = "<unknown>";
  char below[128] = "<unknown>";
  char biome[128] = "<unknown>";
  // `floor`, not a truncating cast: a negative coordinate has to name the cell
  // it is in, exactly as the world does (x = -12.0 is in cell -12, not -11).
  const int bx = static_cast<int>(std::floor(player.x));
  const int by = static_cast<int>(std::floor(player.y));
  const int bz = static_cast<int>(std::floor(player.z));
  if (greeter.api->world_block(greeter.api->host, bx, by, bz, feet, sizeof(feet)) != 0)
    std::snprintf(feet, sizeof(feet), "<none>");
  if (greeter.api->world_block(greeter.api->host, bx, by - 1, bz, below, sizeof(below)) != 0)
    std::snprintf(below, sizeof(below), "<none>");
  if (greeter.api->world_biome(greeter.api->host, bx, by, bz, biome, sizeof(biome)) != 0)
    std::snprintf(biome, sizeof(biome), "<none>");
  static const char* kGamemodes[] = {"survival", "creative", "adventure", "spectator"};
  const char* gamemode =
      player.gamemode >= 0 && player.gamemode <= 3 ? kGamemodes[player.gamemode] : "unknown";
  char line[512];
  std::snprintf(line, sizeof(line),
                "(whereami) %s at %.2f, %.2f, %.2f (vel %.3f, %.3f, %.3f; gamemode %s, "
                "health %.1f), biome %s, feet in %s, standing on %s",
                player.name, player.x, player.y, player.z, player.vx, player.vy, player.vz,
                gamemode, static_cast<double>(player.health), biome, feet, below);
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /pillar [height] — the write-accessor example. It reads the sender's own
// snapshot and writes `height` stone blocks upward from their feet through
// `world_set_block`, which is the engine's `/setblock` path: the client that
// asked gets the same block-update packets any other write produces, and a
// second client holding the chunk sees them too. The command is deliberately
// small — a plugin's write is one call, not a column handle it owns.
void on_pillar_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->player_get == nullptr || greeter.api->world_set_block == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(pillar) this host cannot write blocks");
    return;
  }
  int height = 3;
  if (command->argc > 1) {
    char* end = nullptr;
    const long parsed = std::strtol(command->argv[1], &end, 10);
    if (end == command->argv[1] || *end != '\0' || parsed < 1 || parsed > 16) {
      if (command->reply != nullptr)
        greeter.api->reply(greeter.api->host, command->reply,
                           "(pillar) usage: /pillar [height 1..16]");
      return;
    }
    height = static_cast<int>(parsed);
  }
  const char* target = command->player != nullptr ? command->player : "";
  pmc_player player{};
  player.struct_size = sizeof(pmc_player);
  if (greeter.api->player_get(greeter.api->host, target, &player) != 0) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, "(pillar) the sender is not in play");
    return;
  }
  // The cell the player's feet occupy: the block the write lands in is the one
  // `/whereami` reports as "feet in". `floor`, not a truncating cast, for the
  // negative-coordinate reason `/whereami` documents.
  const int bx = static_cast<int>(std::floor(player.x));
  const int by = static_cast<int>(std::floor(player.y));
  const int bz = static_cast<int>(std::floor(player.z));
  int placed = 0;
  int code = PMC_BLOCK_OK;
  for (int i = 0; i < height; ++i) {
    code = greeter.api->world_set_block(greeter.api->host, bx, by + i, bz, "minecraft:stone");
    if (code != PMC_BLOCK_OK)
      break;
    ++placed;
  }
  char line[256];
  if (code != PMC_BLOCK_OK) {
    std::snprintf(line, sizeof(line), "(pillar) wrote %d block(s), then failed (%d) at %d, %d, %d",
                  placed, code, bx, by + placed, bz);
  } else {
    std::snprintf(line, sizeof(line), "(pillar) placed %d stone block(s) at %d, %d, %d", placed, bx,
                  by, bz);
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /entities — the listing accessor. It counts every entity the engine reports,
// by type, and names the first few players: the reply stays one line however
// crowded the world is, and the counts prove the listing saw more than the
// sender's own connection.
struct EntityTally {
  int total = 0;
  std::map<std::string, int> by_type;
  std::vector<std::string> players;
};

void visit_entity(void* data, const pmc_entity* entity) {
  auto& tally = *static_cast<EntityTally*>(data);
  ++tally.total;
  ++tally.by_type[entity->type];
  if (std::strcmp(entity->type, "minecraft:player") == 0 && tally.players.size() < 4)
    tally.players.push_back(entity->name);
}

void on_entities_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->entities == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entities) this host cannot list entities");
    return;
  }
  EntityTally tally;
  const int visited = greeter.api->entities(greeter.api->host, &visit_entity, &tally);
  std::string line;
  if (visited < 0) {
    line = "(entities) this server has no world";
  } else {
    line =
        "(entities) " + std::to_string(tally.total) + (tally.total == 1 ? " entity" : " entities");
    for (const auto& entry : tally.by_type)
      line += ", " + entry.first + " x" + std::to_string(entry.second);
    if (!tally.players.empty()) {
      line += " (players:";
      for (const std::string& name : tally.players)
        line += " " + name;
      line += ")";
    }
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line.c_str());
}

// One entity by id, through the ABI's own lookup. The write commands use this
// to report the health a hit moved; a plugin that starts from a name gets the
// id from the listing once and then works by id rather than walking it again.
bool find_entity(const Greeter& greeter, int32_t id, pmc_entity* out) {
  if (greeter.api->entity_get == nullptr)
    return false;
  pmc_entity entity{};
  entity.struct_size = sizeof(pmc_entity);
  if (greeter.api->entity_get(greeter.api->host, id, &entity) != PMC_ENTITY_OK)
    return false;
  *out = entity;
  return true;
}

bool parse_entity_id(const char* text, int32_t* out) {
  char* end = nullptr;
  errno = 0;
  const long id = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || errno == ERANGE || id <= 0 || id > 2147483647L)
    return false;
  *out = static_cast<int32_t>(id);
  return true;
}

bool parse_double_arg(const char* text, double* out) {
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value))
    return false;
  *out = value;
  return true;
}

bool parse_positive_float(const char* text, float* out) {
  char* end = nullptr;
  errno = 0;
  const float value = std::strtof(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE || !std::isfinite(value) || value <= 0.0f)
    return false;
  *out = value;
  return true;
}

const char* entity_error(int code) {
  switch (code) {
  case PMC_ENTITY_NOT_FOUND:
    return "no such entity";
  case PMC_ENTITY_UNSUPPORTED:
    return "that entity cannot be written this way";
  case PMC_ENTITY_INVALID:
    return "invalid argument";
  case PMC_ENTITY_REFUSED:
    return "the entity refused the write";
  default:
    return "failed";
  }
}

// /entget <id> — the single-entity lookup beside the listing: one snapshot,
// with the type, display name, position, health, age, velocity and facing the
// ABI reports.
void on_entget_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->entity_get == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entget) this host cannot look up entities");
    return;
  }
  int32_t id = 0;
  if (command->argc < 2 || !parse_entity_id(command->argv[1], &id)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, "(entget) usage: /entget <entity id>");
    return;
  }
  pmc_entity entity{};
  entity.struct_size = sizeof(pmc_entity);
  const int rc = greeter.api->entity_get(greeter.api->host, id, &entity);
  char line[512];
  if (rc != PMC_ENTITY_OK) {
    std::snprintf(line, sizeof(line), "(entget) %s", entity_error(rc));
  } else {
    std::snprintf(line, sizeof(line),
                  "(entget) entity %d: %s \"%s\" at %.2f, %.2f, %.2f "
                  "(health %.1f, age %d, vel %.3f, %.3f, %.3f, yaw %.1f, pitch %.1f)",
                  entity.entity_id, entity.type, entity.name, entity.x, entity.y, entity.z,
                  static_cast<double>(entity.health), entity.age, entity.vx, entity.vy, entity.vz,
                  static_cast<double>(entity.yaw), static_cast<double>(entity.pitch));
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /entdmg <id> <amount> — the damage write. A mob's health moves before the
// reply; a player's hit is queued for that player's own session, so the line
// says "queued" rather than pretending the bar already moved.
void on_entdmg_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->entity_damage == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entdmg) this host cannot damage entities");
    return;
  }
  int32_t id = 0;
  float amount = 0.0f;
  if (command->argc < 3 || !parse_entity_id(command->argv[1], &id) ||
      !parse_positive_float(command->argv[2], &amount)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entdmg) usage: /entdmg <entity id> <amount>");
    return;
  }
  pmc_entity before{};
  const bool had_before = find_entity(greeter, id, &before);
  const int rc = greeter.api->entity_damage(greeter.api->host, id, amount);
  char line[320];
  if (rc != PMC_ENTITY_OK) {
    std::snprintf(line, sizeof(line), "(entdmg) %s", entity_error(rc));
  } else if (had_before && std::strcmp(before.type, "minecraft:player") == 0) {
    std::snprintf(line, sizeof(line), "(entdmg) queued %.1f damage for player %s (entity %d)",
                  static_cast<double>(amount), before.name, id);
  } else {
    pmc_entity after{};
    const bool has_after = find_entity(greeter, id, &after);
    if (had_before && has_after)
      std::snprintf(line, sizeof(line), "(entdmg) entity %d: %.1f -> %.1f", id,
                    static_cast<double>(before.health), static_cast<double>(after.health));
    else
      std::snprintf(line, sizeof(line), "(entdmg) entity %d took %.1f damage", id,
                    static_cast<double>(amount));
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /enttp <id> <x> <y> <z> — the teleport write. The engine sends the viewers an
// authoritative position sync; a player is moved by their own session on its
// next tick.
void on_enttp_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->entity_teleport == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(enttp) this host cannot teleport entities");
    return;
  }
  int32_t id = 0;
  double x = 0.0, y = 0.0, z = 0.0;
  if (command->argc < 5 || !parse_entity_id(command->argv[1], &id) ||
      !parse_double_arg(command->argv[2], &x) || !parse_double_arg(command->argv[3], &y) ||
      !parse_double_arg(command->argv[4], &z)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(enttp) usage: /enttp <entity id> <x> <y> <z>");
    return;
  }
  pmc_entity entity{};
  const bool found = find_entity(greeter, id, &entity);
  const int rc = greeter.api->entity_teleport(greeter.api->host, id, x, y, z);
  char line[320];
  if (rc != PMC_ENTITY_OK) {
    std::snprintf(line, sizeof(line), "(enttp) %s", entity_error(rc));
  } else if (found && std::strcmp(entity.type, "minecraft:player") == 0) {
    std::snprintf(line, sizeof(line), "(enttp) queued player %s (entity %d) to %.2f, %.2f, %.2f",
                  entity.name, id, x, y, z);
  } else {
    std::snprintf(line, sizeof(line), "(enttp) moved entity %d to %.2f, %.2f, %.2f", id, x, y, z);
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /entdespawn <id> — the discard write: no death event, no loot, no experience.
void on_entdespawn_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->entity_despawn == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entdespawn) this host cannot despawn entities");
    return;
  }
  int32_t id = 0;
  if (command->argc < 2 || !parse_entity_id(command->argv[1], &id)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(entdespawn) usage: /entdespawn <entity id>");
    return;
  }
  const int rc = greeter.api->entity_despawn(greeter.api->host, id);
  char line[256];
  if (rc != PMC_ENTITY_OK)
    std::snprintf(line, sizeof(line), "(entdespawn) %s", entity_error(rc));
  else
    std::snprintf(line, sizeof(line), "(entdespawn) discarded entity %d", id);
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// A slot of the player inventory, which is the numbering `inventory_get` and
// `inventory_set` use: 0..8 hotbar, 9..35 main, 36..39 armour, 40 offhand.
bool parse_inventory_slot(const char* text, int* out) {
  char* end = nullptr;
  const long slot = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || slot < 0 || slot > 40)
    return false;
  *out = static_cast<int>(slot);
  return true;
}

const char* inventory_error(int code) {
  switch (code) {
  case PMC_INVENTORY_NO_PLAYER:
    return "no such player";
  case PMC_INVENTORY_OUT_OF_RANGE:
    return "slot or count out of range";
  case PMC_INVENTORY_UNKNOWN_ITEM:
    return "unknown item";
  case PMC_INVENTORY_WRITE_FAILED:
    return "the write could not be queued";
  default:
    return "failed";
  }
}

// /invget <slot> [player] — the inventory read accessor. The answer is a copy
// of the slot the target's session last published; an empty slot is a normal
// answer, not an error.
void on_invget_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->inventory_get == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(invget) this host cannot read inventories");
    return;
  }
  int slot = 0;
  if (command->argc < 2 || !parse_inventory_slot(command->argv[1], &slot)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(invget) usage: /invget <slot 0..40> [player]");
    return;
  }
  const char* target =
      command->argc > 2 ? command->argv[2] : (command->player != nullptr ? command->player : "");
  pmc_item item{};
  item.struct_size = sizeof(pmc_item);
  const int rc = greeter.api->inventory_get(greeter.api->host, target, slot, &item);
  if (rc != PMC_INVENTORY_OK) {
    const std::string line = std::string("(invget) ") + inventory_error(rc);
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply, line.c_str());
    return;
  }
  char line[320];
  if (item.count <= 0) {
    std::snprintf(line, sizeof(line), "(invget) %s slot %d is empty", target, slot);
  } else {
    std::snprintf(line, sizeof(line), "(invget) %s slot %d: %s x%d (damage %d, max %d)", target,
                  slot, item.name, item.count, item.damage, item.max_stack_size);
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /invset <slot> <item> [count] [player] — the inventory write accessor. The
// call queues the write for the target's own session, which applies it on its
// next tick: the reply says "queued", and the stack becomes readable on the
// next `inventory_get` once that tick has run. `count` 0 clears the slot. A
// numeric third token is the count; a non-numeric one is a player name.
void on_invset_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  if (greeter.api->inventory_set == nullptr) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(invset) this host cannot write inventories");
    return;
  }
  int slot = 0;
  if (command->argc < 3 || !parse_inventory_slot(command->argv[1], &slot)) {
    if (command->reply != nullptr)
      greeter.api->reply(greeter.api->host, command->reply,
                         "(invset) usage: /invset <slot 0..40> <item> [count] [player]");
    return;
  }
  const char* item = command->argv[2];
  long count = 1;
  int next = 3;
  if (command->argc > 3) {
    char* end = nullptr;
    const long parsed = std::strtol(command->argv[3], &end, 10);
    if (end != command->argv[3] && *end == '\0') {
      // The engine validates against the item's own cap; this only keeps the
      // long -> int conversion below well-defined.
      if (parsed < 0 || parsed > 2147483647L) {
        if (command->reply != nullptr)
          greeter.api->reply(greeter.api->host, command->reply,
                             "(invset) usage: /invset <slot 0..40> <item> [count] [player]");
        return;
      }
      count = parsed;
      ++next;
    }
  }
  const char* target = command->argc > next ? command->argv[next]
                                            : (command->player != nullptr ? command->player : "");
  const int rc =
      greeter.api->inventory_set(greeter.api->host, target, slot, item, static_cast<int>(count));
  char line[320];
  if (rc == PMC_INVENTORY_OK) {
    if (count == 0)
      std::snprintf(line, sizeof(line), "(invset) queued slot %d of %s to be cleared", slot,
                    target);
    else
      std::snprintf(line, sizeof(line), "(invset) queued slot %d of %s -> %s x%ld", slot, target,
                    item, count);
  } else {
    std::snprintf(line, sizeof(line), "(invset) %s", inventory_error(rc));
  }
  if (command->reply != nullptr)
    greeter.api->reply(greeter.api->host, command->reply, line);
}

// /opcmd requires operator level 3. The host refuses it before this handler for
// a lower-level operator, then answers with a plain denial line.
void on_op_command(void* data, pmc_command* command) {
  auto& greeter = *static_cast<Greeter*>(data);
  int level = -1;
  if (greeter.api->player_op_level != nullptr && command->player != nullptr)
    level = greeter.api->player_op_level(greeter.api->host, command->player);
  if (command->reply != nullptr && greeter.api->reply != nullptr) {
    const std::string line = std::string("(opcmd) operator level 3 command ran for ") +
                             (command->player != nullptr ? command->player : "?") + " (level " +
                             std::to_string(level) + ")";
    greeter.api->reply(greeter.api->host, command->reply, line.c_str());
  }
}

void on_heartbeat(void* data) {
  auto& greeter = *static_cast<Greeter*>(data);
  const std::string text =
      "heartbeat at tick " + std::to_string(greeter.api->tick_count(greeter.api->host));
  log_line(greeter, PMC_LOG_INFO, text);
}

// The async scheduler's example: a deliberate sleep that must never run on the
// tick or session thread. Unload waits for it before closing the module.
void on_async_task(void* data) {
  auto& greeter = *static_cast<Greeter*>(data);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  log_line(greeter, PMC_LOG_INFO, "async task ran on the worker thread");
}

// Queued behind the sleeping task, then cancelled. This line appearing would
// mean `cancel` failed to drop an async task that had not started.
void on_cancelled_async(void* data) {
  auto& greeter = *static_cast<Greeter*>(data);
  log_line(greeter, PMC_LOG_INFO, "cancelled async task ran");
}

void on_server_start(void* data, pmc_event* event) {
  (void)event;
  auto& greeter = *static_cast<Greeter*>(data);
  log_line(greeter, PMC_LOG_INFO,
           std::string("server started on ") + greeter.api->server_version(greeter.api->host));
}

int plugin_load(const pmc_host_api* api, void** plugin_data) {
  auto greeter = new Greeter();
  greeter->api = api;
  // The host reads config.toml before on_load, so these are available now.
  if (api->config_string != nullptr) {
    const char* greeting = api->config_string(api->host, "greeting");
    if (greeting != nullptr)
      greeter->greeting = greeting;
  }
  if (api->config_int != nullptr) {
    const int64_t ticks = api->config_int(api->host, "heartbeat_ticks", 20);
    if (ticks >= 1 && ticks <= 1200)
      greeter->heartbeat_ticks = static_cast<uint32_t>(ticks);
  }
  if (api->config_bool != nullptr)
    greeter->debug = api->config_bool(api->host, "debug", 0) != 0;
  if (api->on(api->host, PMC_EVENT_PLAYER_CHAT, &on_chat, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_PLAYER_JOIN, &on_join, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_PLAYER_LEAVE, &on_leave, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_BLOCK_PLACE, &on_block, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_BLOCK_BREAK, &on_block, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_SERVER_START, &on_server_start, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_PLAYER_CHAT, &on_chat_audit, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_ITEM_USE_ON, &on_item_use, greeter) != 0 ||
      api->on(api->host, PMC_EVENT_ENTITY_DEATH, &on_entity_death, greeter) != 0 ||
      api->command(api->host, "itemguard", "/itemguard <allow|deny>", &on_itemguard_command,
                   greeter) != 0 ||
      api->command(api->host, "blockguard", "/blockguard <place|break> <allow|deny>",
                   &on_blockguard_command, greeter) != 0 ||
      api->command(api->host, "greet", "/greet [name]", &on_greet_command, greeter) != 0 ||
      api->command(api->host, "whereami", "/whereami [player]", &on_whereami_command, greeter) !=
          0 ||
      api->command(api->host, "pillar", "/pillar [height]", &on_pillar_command, greeter) != 0 ||
      api->command(api->host, "entities", "/entities", &on_entities_command, greeter) != 0 ||
      api->command(api->host, "invget", "/invget <slot 0..40> [player]", &on_invget_command,
                   greeter) != 0 ||
      api->command(api->host, "invset", "/invset <slot 0..40> <item> [count] [player]",
                   &on_invset_command, greeter) != 0 ||
      api->command(api->host, "entget", "/entget <entity id>", &on_entget_command, greeter) != 0 ||
      api->command(api->host, "entdmg", "/entdmg <entity id> <amount>", &on_entdmg_command,
                   greeter) != 0 ||
      api->command(api->host, "enttp", "/enttp <entity id> <x> <y> <z>", &on_enttp_command,
                   greeter) != 0 ||
      api->command(api->host, "deaths", "/deaths", &on_deaths_command, greeter) != 0 ||
      api->command(api->host, "worldclock", "/worldclock", &on_worldclock_command, greeter) != 0 ||
      api->command(api->host, "setworldtime", "/setworldtime <ticks>", &on_setworldtime_command,
                   greeter) != 0 ||
      api->command(api->host, "setweather", "/setweather <clear|rain|thunder> [ticks]",
                   &on_setweather_command, greeter) != 0 ||
      api->command(api->host, "dm", "/dm <player> <message>", &on_dm_command, greeter) != 0 ||
      api->command(api->host, "broadcast", "/broadcast <message>", &on_broadcast_command,
                   greeter) != 0 ||
      api->command(api->host, "online", "/online", &on_online_command, greeter) != 0 ||
      api->command(api->host, "config", "/config <key> [value]", &on_config_command, greeter) !=
          0 ||
      api->command(api->host, "find", "/find <name>", &on_find_command, greeter) != 0 ||
      api->command(api->host, "setgm", "/setgm <player> <mode>", &on_setgm_command, greeter) != 0 ||
      api->command(api->host, "heal", "/heal <player> [health]", &on_heal_command, greeter) != 0 ||
      api->command(api->host, "feed", "/feed <player> [food] [saturation]", &on_feed_command,
                   greeter) != 0 ||
      api->command_level == nullptr ||
      api->command_level(api->host, "opcmd", "/opcmd", 3, &on_op_command, greeter) != 0 ||
      api->command(api->host, "entdespawn", "/entdespawn <entity id>", &on_entdespawn_command,
                   greeter) != 0) {
    delete greeter;
    return 1;
  }
  // Every `heartbeat_ticks` server ticks (about a second by default). The id is
  // kept so on_disable could cancel it, and unloading drops it anyway.
  greeter->heartbeat = api->schedule(api->host, greeter->heartbeat_ticks, greeter->heartbeat_ticks,
                                     &on_heartbeat, greeter);
  *plugin_data = greeter;
  return 0;
}

void plugin_enable(void* data) {
  auto& greeter = *static_cast<Greeter*>(data);
  log_line(greeter, PMC_LOG_INFO, "enabled");
  // The host creates this directory before on_load. A plugin that has config
  // or saved state keeps it here, not beside the module binary.
  if (greeter.api->data_dir != nullptr) {
    const char* dir = greeter.api->data_dir(greeter.api->host);
    if (dir != nullptr)
      log_line(greeter, PMC_LOG_INFO, std::string("data dir: ") + dir);
  }
  char config_line[400];
  std::snprintf(config_line, sizeof(config_line), "config: greeting=%s heartbeat_ticks=%u debug=%d",
                greeter.greeting.empty() ? "(default)" : greeter.greeting.c_str(),
                greeter.heartbeat_ticks, greeter.debug ? 1 : 0);
  log_line(greeter, PMC_LOG_INFO, config_line);
  if (greeter.api->schedule_async != nullptr) {
    greeter.async_task = greeter.api->schedule_async(greeter.api->host, &on_async_task, &greeter);
    const uint64_t queued =
        greeter.api->schedule_async(greeter.api->host, &on_cancelled_async, &greeter);
    if (queued != 0 && greeter.api->cancel != nullptr)
      greeter.api->cancel(greeter.api->host, queued);
  }
}

void plugin_disable(void* data) {
  log_line(*static_cast<Greeter*>(data), PMC_LOG_INFO, "disabled");
}

void plugin_unload(void* data) {
  delete static_cast<Greeter*>(data);
}

const pmc_plugin kPlugin = {
    sizeof(pmc_plugin),
    PMC_PLUGIN_ABI_VERSION,
    "greeter",
    "1.13.0",
    "+MC",
    "Example plugin: filters chat, welcomes joins, logs leaves, answers /greet, /whereami, "
    "/pillar, /entities, /entget, /find, /invget, /invset, /entdmg, /enttp, /entdespawn, "
    "/blockguard, /itemguard, /worldclock, /setworldtime, /setweather, /dm, /broadcast, "
    "/online, /config, /setgm, /heal and /feed, logs a heartbeat, reads and writes "
    "greeting/heartbeat_ticks/debug in its config.toml, "
    "and runs one async task at enable.",
    &plugin_load,
    &plugin_enable,
    &plugin_disable,
    &plugin_unload,
};

} // namespace

extern "C" PMC_PLUGIN_EXPORT const pmc_plugin* pmc_plugin_entry(void) {
  return &kPlugin;
}
