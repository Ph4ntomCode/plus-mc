// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 2: the pinned WASM runtime.
//
// `module.cpp` decides whether a module's imports can be satisfied. This file
// is the other half: it parses the sections the gate only bounds-checks,
// instantiates the module against a sandboxed memory and the `plusmc:host`
// table, and executes its lifecycle exports under hard budgets — instructions,
// memory pages, call depth, wall-clock time, and the bytes a plugin may hand
// the host.
//
// It is an interpreter on purpose: no JIT and no fetched runtime, so a build is
// reproducible and the code a plugin runs is code this repository contains.
// `Host` is the seam a wasmtime backend can replace later without changing the
// ABI a plugin sees.
//
// The interpreter hosts the MVP numeric/control/memory subset plus funcref
// tables, element segments, reference types, typed `select`, saturating
// conversions and bulk memory. `externref`, SIMD, threads, exceptions, tail
// calls and multi-value results are refused at load with the feature named,
// and every body is walked once at load so a module outside the subset never
// half-executes. Full static type validation is not performed — that is a
// validator's job, not a sandbox's — so an invalid stack use traps with a
// named reason instead of reading outside the machine.

#include "wasm/runtime.h"

#include "wasm/component_run.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "converter/report_json.h"
#include "wasm/module.h"
#include "world/nbt.h"

namespace nbt = mc::world::nbt;

namespace mc::wasm {

namespace detail = mc::converter::detail;

namespace {

constexpr std::size_t kPageBytes = 65536;
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::size_t kMaxTypes = 100000;
constexpr std::size_t kMaxFunctions = 100000;
constexpr std::size_t kMaxLocals = 100000;
constexpr std::size_t kMaxReportedLines = 200;
// The full-surface fixture emits more than 200 host calls; keep its final
// overlay flag in the JSON report while still bounding the list.
constexpr std::size_t kMaxReportedHostCalls = 256;
// A table is an index space the plugin can grow; keep it in the same order of
// magnitude as the linear-memory budget rather than letting a header allocate
// four gigabytes of function references.
constexpr std::uint32_t kMaxTableEntries = 1000000;

// ---------------------------------------------------------------------------
// The two ways a run ends early. `Refuse` is a load-time verdict (the module is
// not something this build will host); `Trap` is a fault in a module that was
// hosted; `Budget` is a limit doing its job.
// ---------------------------------------------------------------------------
enum class StopKind { Refuse, Trap, Budget };

struct Stop {
  std::string message;
  StopKind kind = StopKind::Trap;
};

[[noreturn]] void trap(const std::string& message) {
  throw Stop{message, StopKind::Trap};
}

[[noreturn]] void budget(const std::string& message) {
  throw Stop{message, StopKind::Budget};
}

// Pumpkin's `common.locale` enum orders `en-us` at index 25. The runtime uses
// that index for the English fallback required by i18n.translate.
constexpr std::int32_t kEnglishLocale = 25;

struct UuidValue {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
};

bool uuid_parse_text(std::string_view text, UuidValue* out) {
  if (text.size() != 36)
    return false;
  for (std::size_t position : {8u, 13u, 18u, 23u}) {
    if (text[position] != '-')
      return false;
  }
  const auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  std::string digits;
  digits.reserve(32);
  for (char c : text) {
    if (c == '-')
      continue;
    if (hex(c) < 0)
      return false;
    digits.push_back(c);
  }
  if (digits.size() != 32)
    return false;
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  for (std::size_t i = 0; i < 16; ++i)
    high = (high << 4) | static_cast<std::uint64_t>(hex(digits[i]));
  for (std::size_t i = 16; i < 32; ++i)
    low = (low << 4) | static_cast<std::uint64_t>(hex(digits[i]));
  out->high = high;
  out->low = low;
  return true;
}

std::string uuid_format(const UuidValue& id) {
  char text[37];
  std::snprintf(text, sizeof(text), "%08llx-%04llx-%04llx-%04llx-%012llx",
                static_cast<unsigned long long>(id.high >> 32),
                static_cast<unsigned long long>((id.high >> 16) & 0xffff),
                static_cast<unsigned long long>(id.high & 0xffff),
                static_cast<unsigned long long>((id.low >> 48) & 0xffff),
                static_cast<unsigned long long>(id.low & 0xffffffffffffULL));
  return std::string(text);
}

UuidValue uuid_generate_value() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  UuidValue id{engine(), engine()};
  // RFC 4122 version 4, variant 1 (10xx in the high bits of the clock seq).
  id.high = (id.high & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;
  id.low = (id.low & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;
  return id;
}

void append_utf8(std::string* out, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    out->push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    out->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    out->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

bool parse_json_string(std::string_view text, std::size_t* position, std::string* out) {
  if (*position >= text.size() || text[*position] != '"')
    return false;
  ++*position;
  out->clear();
  while (*position < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[(*position)++]);
    if (c == '"')
      return true;
    if (c < 0x20)
      return false;
    if (c != '\\') {
      out->push_back(static_cast<char>(c));
      continue;
    }
    if (*position >= text.size())
      return false;
    const char escape = text[(*position)++];
    switch (escape) {
    case '"':
      out->push_back('"');
      break;
    case '\\':
      out->push_back('\\');
      break;
    case '/':
      out->push_back('/');
      break;
    case 'b':
      out->push_back('\b');
      break;
    case 'f':
      out->push_back('\f');
      break;
    case 'n':
      out->push_back('\n');
      break;
    case 'r':
      out->push_back('\r');
      break;
    case 't':
      out->push_back('\t');
      break;
    case 'u': {
      if (*position + 4 > text.size())
        return false;
      std::uint32_t code_point = 0;
      for (int i = 0; i < 4; ++i) {
        const char digit = text[(*position)++];
        int value = -1;
        if (digit >= '0' && digit <= '9')
          value = digit - '0';
        else if (digit >= 'a' && digit <= 'f')
          value = digit - 'a' + 10;
        else if (digit >= 'A' && digit <= 'F')
          value = digit - 'A' + 10;
        if (value < 0)
          return false;
        code_point = (code_point << 4) | static_cast<std::uint32_t>(value);
      }
      append_utf8(out, code_point);
      break;
    }
    default:
      return false;
    }
  }
  return false;
}

bool parse_flat_json(std::string_view text, std::map<std::string, std::string>* out) {
  std::size_t position = 0;
  const auto skip_space = [&] {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\n' ||
                                      text[position] == '\r' || text[position] == '\t'))
      ++position;
  };
  skip_space();
  if (position >= text.size() || text[position] != '{')
    return false;
  ++position;
  skip_space();
  if (position < text.size() && text[position] == '}') {
    ++position;
    skip_space();
    return position == text.size();
  }
  while (position < text.size()) {
    std::string key;
    std::string value;
    if (!parse_json_string(text, &position, &key))
      return false;
    skip_space();
    if (position >= text.size() || text[position] != ':')
      return false;
    ++position;
    skip_space();
    if (!parse_json_string(text, &position, &value))
      return false;
    (*out)[key] = value;
    skip_space();
    if (position >= text.size())
      return false;
    if (text[position] == ',') {
      ++position;
      skip_space();
      continue;
    }
    if (text[position] == '}') {
      ++position;
      skip_space();
      return position == text.size();
    }
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

enum class ValType : std::uint8_t {
  I32 = 0x7f,
  I64 = 0x7e,
  F32 = 0x7d,
  F64 = 0x7c,
  FuncRef = 0x70,
};

const char* valtype_text(ValType type) {
  switch (type) {
  case ValType::I32:
    return "i32";
  case ValType::I64:
    return "i64";
  case ValType::F32:
    return "f32";
  case ValType::F64:
    return "f64";
  case ValType::FuncRef:
    return "funcref";
  }
  return "?";
}

struct Value {
  ValType type = ValType::I32;
  // The value's bytes: the low 32 bits for i32/f32, all 64 for i64/f64.
  std::uint64_t bits = 0;
};

Value make_i32(std::uint32_t value) {
  return Value{ValType::I32, value};
}
Value make_i64(std::uint64_t value) {
  return Value{ValType::I64, value};
}
Value make_f32(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return Value{ValType::F32, bits};
}
Value make_f64(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return Value{ValType::F64, bits};
}

std::uint32_t as_u32(const Value& value) {
  return static_cast<std::uint32_t>(value.bits);
}
std::int32_t as_i32(const Value& value) {
  return static_cast<std::int32_t>(as_u32(value));
}
std::uint64_t as_u64(const Value& value) {
  return value.bits;
}
std::int64_t as_i64(const Value& value) {
  return static_cast<std::int64_t>(value.bits);
}
float as_f32(const Value& value) {
  const std::uint32_t bits = as_u32(value);
  float out = 0;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}
double as_f64(const Value& value) {
  const std::uint64_t bits = value.bits;
  double out = 0;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

Value zero_of(ValType type) {
  return Value{type, 0};
}

// ---------------------------------------------------------------------------
// Bounded reader. Every length is checked against what remains; nothing
// allocates from a claim before the claim fits.
// ---------------------------------------------------------------------------

class Reader {
public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool u8(std::uint8_t* out) {
    if (position_ >= size_)
      return false;
    *out = data_[position_++];
    return true;
  }

  bool u32(std::uint32_t* out) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
      std::uint8_t byte = 0;
      if (!u8(&byte))
        return false;
      if (shift == 28 && (byte & 0xf0) != 0)
        return false;
      result |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) {
        *out = result;
        return true;
      }
    }
    return false;
  }

  bool s64(std::uint64_t* out) {
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (;;) {
      if (shift >= 64)
        return false;
      std::uint8_t byte = 0;
      if (!u8(&byte))
        return false;
      result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
      shift += 7;
      if ((byte & 0x80) == 0) {
        if (shift < 64 && (byte & 0x40) != 0)
          result |= ~std::uint64_t{0} << shift;
        *out = result;
        return true;
      }
    }
  }

  bool bytes(std::size_t count, const std::uint8_t** out) {
    if (count > remaining())
      return false;
    *out = data_ + position_;
    position_ += count;
    return true;
  }

  bool skip(std::size_t count) {
    if (count > remaining())
      return false;
    position_ += count;
    return true;
  }

  bool name(std::string* out) {
    std::uint32_t length = 0;
    if (!u32(&length) || length > kMaxNameBytes)
      return false;
    const std::uint8_t* start = nullptr;
    if (!bytes(length, &start))
      return false;
    out->assign(reinterpret_cast<const char*>(start), length);
    return true;
  }

  std::size_t remaining() const {
    return size_ - position_;
  }
  std::size_t position() const {
    return position_;
  }
  const std::uint8_t* data() const {
    return data_;
  }
  std::size_t size() const {
    return size_;
  }
  void advance(std::size_t count) {
    position_ += count;
  }

  bool sub(std::uint32_t length, Reader* out) const {
    if (length > remaining())
      return false;
    *out = Reader(data_ + position_, length);
    return true;
  }

private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

// ---------------------------------------------------------------------------
// The parsed module
// ---------------------------------------------------------------------------

struct FuncType {
  std::vector<ValType> params;
  bool has_result = false;
  ValType result = ValType::I32;
};

struct BlockEnds {
  std::uint32_t start = 0; // offset of the `block`/`loop`/`if` opcode
  std::uint32_t body = 0;  // offset after the block type
  std::uint32_t else_body = 0;
  std::uint32_t end = 0; // offset after the matching `end`
  std::uint8_t kind = 0; // 0 block, 1 loop, 2 if
  bool has_else = false;
  std::size_t arity = 0;
};

struct FuncBody {
  std::uint32_t type_index = 0;
  std::vector<ValType> locals;
  std::size_t body_offset = 0;
  std::size_t body_size = 0;
  std::vector<BlockEnds> blocks;
};

enum class HostId {
  Broadcast,
  ConfigGet,
  ConfigGetString,
  ConfigKeys,
  ConfigPair,
  ConfigSet,
  EntityFind,
  Log,
  LogLevel,
  LogTracing,
  PlayerChat,
  PlayerCount,
  PlayerMessage,
  PumpkinPlayerGetId,
  PumpkinPlayerGetName,
  PumpkinPlayerGetPosition,
  PumpkinPlayerGetYaw,
  PumpkinPlayerGetPitch,
  PumpkinPlayerGetGamemode,
  PumpkinPlayerGetHealth,
  PumpkinPlayerGetMaxHealth,
  PumpkinPlayerGetFoodLevel,
  PumpkinPlayerGetSaturation,
  PumpkinPlayerGetExhaustion,
  PumpkinPlayerGetAbsorption,
  PumpkinPlayerGetExperienceLevel,
  PumpkinPlayerGetExperienceProgress,
  PumpkinPlayerGetExperiencePoints,
  PumpkinPlayerGetCameraEntityId,
  PumpkinPlayerChat,
  PumpkinPlayerSendSystemMessage,
  PumpkinServerAllPlayers,
  PumpkinServerBroadcast,
  PumpkinServerDefaultGamemode,
  PumpkinServerDifficulty,
  PumpkinServerHasWorld,
  PumpkinServerMaxPlayers,
  PumpkinServerMotd,
  PumpkinServerOnlineMode,
  PumpkinServerPlayer,
  PumpkinServerPlayerCountInWorld,
  PumpkinServerPlayersInWorld,
  PumpkinServerSimulationDistance,
  PumpkinServerViewDistance,
  PumpkinServerWhitelistEnabled,
  PumpkinTextAddChild,
  PumpkinTextAddText,
  PumpkinTextText,
  PumpkinTextColorNamed,
  PumpkinTextColorRgb,
  PumpkinTextBold,
  PumpkinTextItalic,
  PumpkinTextUnderlined,
  PumpkinTextStrikethrough,
  PumpkinTextObfuscated,
  PumpkinTextInsertion,
  PumpkinTextFont,
  PumpkinTextClickOpenUrl,
  PumpkinTextClickOpenFile,
  PumpkinTextClickRunCommand,
  PumpkinTextClickSuggestCommand,
  PumpkinTextClickChangePage,
  PumpkinTextClickCopyToClipboard,
  PumpkinTextHoverShowText,
  PumpkinCommandExecute,
  PumpkinCommandNew,
  PumpkinCommandThen,
  PumpkinCommandNodeArgument,
  PumpkinCommandNodeExecute,
  PumpkinCommandNodeLiteral,
  PumpkinCommandNodeRequire,
  PumpkinCommandNodeSuggest,
  PumpkinCommandNodeThen,
  PumpkinContextGetDataFolder,
  PumpkinContextMarketplaceMetadata,
  PumpkinContextRegisterCommand,
  PumpkinContextRegisterEvent,
  PumpkinContextRegisterPermission,
  PumpkinContextServer,
  PumpkinLog,
  PumpkinServerWorld,
  PumpkinWorldGetBlock,
  Reply,
  ResourceDrop,
  ResourceDropCommand,
  ResourceDropCommandNode,
  ResourceDropContext,
  ResourceDropPlayer,
  ResourceDropServer,
  ResourceDropText,
  ResourceDropWorld,
  SchedulerCancel,
  SchedulerScheduleDelayed,
  UuidGenerate,
  UuidParse,
  UuidToString,
  I18nLoadTranslations,
  I18nTranslate,
  ItemStackNew,
  ItemStackGetRegistryKey,
  ItemStackGetCount,
  ItemStackSetCount,
  ItemStackGetMaxCount,
  ItemStackClone,
  ItemStackWithCount,
  ItemStackIsSimilar,
  ItemStackGetDamage,
  ItemStackSetDamage,
  ItemStackGetMaxDamage,
  ItemStackIsDamageable,
  ItemStackIsUnbreakable,
  ItemStackSetUnbreakable,
  ItemStackGetRepairCost,
  ItemStackSetRepairCost,
  ItemStackGetItemModel,
  ItemStackSetItemModel,
  ItemStackGetEnchantmentGlintOverride,
  ItemStackSetEnchantmentGlintOverride,
  ItemStackGetAttributeModifiers,
  ItemStackAddAttributeModifier,
  ItemStackRemoveAttributeModifiers,
  ItemStackClearAttributeModifiers,
  ItemStackSetCustomData,
  ItemStackGetCustomData,
  ItemStackRemoveCustomData,
  ItemStackHasCustomData,
  ItemStackGetComponents,
  ItemStackSetComponent,
  ItemStackRemoveComponent,
  ItemStackGetEnchantments,
  ItemStackAddEnchantment,
  ItemStackRemoveEnchantment,
  ItemStackGetCustomEnchantments,
  ItemStackAddCustomEnchantment,
  ItemStackRemoveCustomEnchantment,
  ItemStackGetCustomEnchantmentLevel,
  ItemStackHasCustomEnchantment,
  ItemStackGetLore,
  ItemStackSetLore,
  ItemStackAddLore,
  ItemStackGetCustomName,
  ItemStackSetCustomName,
  ResourceDropItem,
  SchedulerScheduleRepeating,
  Stub,
  TaskReturn,
  TaskReturnVoid,
  WorldGetBlock,
  WorldGetBlockId,
  WorldGetBlockInfo,
  WorldIsRaining,
  WorldIsThundering,
  WorldSetBlock,
  PumpkinWorldSetBlock,
  PumpkinWorldSetBlockAsync,
  PumpkinWorldSetBlockSpill,
  WaitableSetNew,
  WaitableSetJoin,
  WaitableSetWait,
  WorldSetTime,
  WorldTime,
  WorldWeather,
};

bool host_id(const std::string& name, HostId* out) {
  static const std::map<std::string, HostId> kIds = {
      {"broadcast", HostId::Broadcast},
      {"config_get", HostId::ConfigGet},
      {"config_get_string", HostId::ConfigGetString},
      {"config_keys", HostId::ConfigKeys},
      {"config_pair", HostId::ConfigPair},
      {"config_set", HostId::ConfigSet},
      {"entity_find", HostId::EntityFind},
      {"log", HostId::Log},
      {"log_level", HostId::LogLevel},
      {"log_tracing", HostId::LogTracing},
      {"player_chat", HostId::PlayerChat},
      {"player_count", HostId::PlayerCount},
      {"player_message", HostId::PlayerMessage},
      // Internal component bindings: `pumpkin_log` carries the WIT
      // `log(level, message)` shape, and the two canonical built-ins are bound
      // only when the component linker produced them, never as direct
      // `plusmc:host` imports.
      {"pumpkin_context_get_data_folder", HostId::PumpkinContextGetDataFolder},
      {"pumpkin_context_get_server", HostId::PumpkinContextServer},
      {"pumpkin_context_marketplace_metadata", HostId::PumpkinContextMarketplaceMetadata},
      {"pumpkin_context_register_command", HostId::PumpkinContextRegisterCommand},
      {"pumpkin_context_register_permission", HostId::PumpkinContextRegisterPermission},
      {"command_execute", HostId::PumpkinCommandExecute},
      {"command_new", HostId::PumpkinCommandNew},
      {"command_then", HostId::PumpkinCommandThen},
      {"command_node_argument", HostId::PumpkinCommandNodeArgument},
      {"command_node_execute", HostId::PumpkinCommandNodeExecute},
      {"command_node_literal", HostId::PumpkinCommandNodeLiteral},
      {"command_node_require", HostId::PumpkinCommandNodeRequire},
      {"command_node_suggest", HostId::PumpkinCommandNodeSuggest},
      {"command_node_then", HostId::PumpkinCommandNodeThen},
      {"pumpkin_context_register_event", HostId::PumpkinContextRegisterEvent},
      {"pumpkin_log", HostId::PumpkinLog},
      {"player_get_id", HostId::PumpkinPlayerGetId},
      {"player_get_name", HostId::PumpkinPlayerGetName},
      {"player_get_position", HostId::PumpkinPlayerGetPosition},
      {"player_get_yaw", HostId::PumpkinPlayerGetYaw},
      {"player_get_pitch", HostId::PumpkinPlayerGetPitch},
      {"player_get_gamemode", HostId::PumpkinPlayerGetGamemode},
      {"player_get_health", HostId::PumpkinPlayerGetHealth},
      {"player_get_max_health", HostId::PumpkinPlayerGetMaxHealth},
      {"player_get_food_level", HostId::PumpkinPlayerGetFoodLevel},
      {"player_get_saturation", HostId::PumpkinPlayerGetSaturation},
      {"player_get_exhaustion", HostId::PumpkinPlayerGetExhaustion},
      {"player_get_absorption", HostId::PumpkinPlayerGetAbsorption},
      {"player_get_experience_level", HostId::PumpkinPlayerGetExperienceLevel},
      {"player_get_experience_progress", HostId::PumpkinPlayerGetExperienceProgress},
      {"player_get_experience_points", HostId::PumpkinPlayerGetExperiencePoints},
      {"player_get_camera_entity_id", HostId::PumpkinPlayerGetCameraEntityId},
      {"pumpkin_player_chat", HostId::PumpkinPlayerChat},
      {"pumpkin_player_send_system_message", HostId::PumpkinPlayerSendSystemMessage},
      {"pumpkin_server_broadcast", HostId::PumpkinServerBroadcast},
      {"pumpkin_server_default_gamemode", HostId::PumpkinServerDefaultGamemode},
      {"pumpkin_server_difficulty", HostId::PumpkinServerDifficulty},
      {"pumpkin_server_has_world", HostId::PumpkinServerHasWorld},
      {"pumpkin_server_max_players", HostId::PumpkinServerMaxPlayers},
      {"pumpkin_server_online_mode", HostId::PumpkinServerOnlineMode},
      {"pumpkin_server_player_by_name", HostId::PumpkinServerPlayer},
      {"server_all_players", HostId::PumpkinServerAllPlayers},
      {"server_player_count_in_world", HostId::PumpkinServerPlayerCountInWorld},
      {"server_players_in_world", HostId::PumpkinServerPlayersInWorld},
      {"pumpkin_text_add_child", HostId::PumpkinTextAddChild},
      {"pumpkin_text_add_text", HostId::PumpkinTextAddText},
      {"pumpkin_text_text", HostId::PumpkinTextText},
      {"pumpkin_text_color_named", HostId::PumpkinTextColorNamed},
      {"pumpkin_text_color_rgb", HostId::PumpkinTextColorRgb},
      {"pumpkin_text_bold", HostId::PumpkinTextBold},
      {"pumpkin_text_italic", HostId::PumpkinTextItalic},
      {"pumpkin_text_underlined", HostId::PumpkinTextUnderlined},
      {"pumpkin_text_strikethrough", HostId::PumpkinTextStrikethrough},
      {"pumpkin_text_obfuscated", HostId::PumpkinTextObfuscated},
      {"pumpkin_text_insertion", HostId::PumpkinTextInsertion},
      {"pumpkin_text_font", HostId::PumpkinTextFont},
      {"pumpkin_text_click_open_url", HostId::PumpkinTextClickOpenUrl},
      {"pumpkin_text_click_open_file", HostId::PumpkinTextClickOpenFile},
      {"pumpkin_text_click_run_command", HostId::PumpkinTextClickRunCommand},
      {"pumpkin_text_click_suggest_command", HostId::PumpkinTextClickSuggestCommand},
      {"pumpkin_text_click_change_page", HostId::PumpkinTextClickChangePage},
      {"pumpkin_text_click_copy_to_clipboard", HostId::PumpkinTextClickCopyToClipboard},
      {"pumpkin_text_hover_show_text", HostId::PumpkinTextHoverShowText},
      {"pumpkin_server_world_by_name", HostId::PumpkinServerWorld},
      {"pumpkin_world_get_block", HostId::PumpkinWorldGetBlock},
      {"reply", HostId::Reply},
      {"resource_drop", HostId::ResourceDrop},
      {"resource_drop_command", HostId::ResourceDropCommand},
      {"resource_drop_command_node", HostId::ResourceDropCommandNode},
      {"resource_drop_context", HostId::ResourceDropContext},
      {"resource_drop_player", HostId::ResourceDropPlayer},
      {"resource_drop_server", HostId::ResourceDropServer},
      {"resource_drop_text", HostId::ResourceDropText},
      {"resource_drop_world", HostId::ResourceDropWorld},
      {"scheduler_cancel", HostId::SchedulerCancel},
      {"scheduler_schedule_delayed", HostId::SchedulerScheduleDelayed},
      {"scheduler_schedule_repeating", HostId::SchedulerScheduleRepeating},
      {"uuid_generate", HostId::UuidGenerate},
      {"uuid_parse", HostId::UuidParse},
      {"uuid_to_string", HostId::UuidToString},
      {"i18n_load_translations", HostId::I18nLoadTranslations},
      {"i18n_translate", HostId::I18nTranslate},
      {"item_stack_new", HostId::ItemStackNew},
      {"item_stack_get_registry_key", HostId::ItemStackGetRegistryKey},
      {"item_stack_get_count", HostId::ItemStackGetCount},
      {"item_stack_set_count", HostId::ItemStackSetCount},
      {"item_stack_get_max_count", HostId::ItemStackGetMaxCount},
      {"item_stack_clone", HostId::ItemStackClone},
      {"item_stack_with_count", HostId::ItemStackWithCount},
      {"item_stack_is_similar", HostId::ItemStackIsSimilar},
      {"item_stack_get_damage", HostId::ItemStackGetDamage},
      {"item_stack_set_damage", HostId::ItemStackSetDamage},
      {"item_stack_get_max_damage", HostId::ItemStackGetMaxDamage},
      {"item_stack_is_damageable", HostId::ItemStackIsDamageable},
      {"item_stack_is_unbreakable", HostId::ItemStackIsUnbreakable},
      {"item_stack_set_unbreakable", HostId::ItemStackSetUnbreakable},
      {"item_stack_get_repair_cost", HostId::ItemStackGetRepairCost},
      {"item_stack_set_repair_cost", HostId::ItemStackSetRepairCost},
      {"item_stack_get_item_model", HostId::ItemStackGetItemModel},
      {"item_stack_set_item_model", HostId::ItemStackSetItemModel},
      {"item_stack_get_enchantment_glint_override", HostId::ItemStackGetEnchantmentGlintOverride},
      {"item_stack_set_enchantment_glint_override", HostId::ItemStackSetEnchantmentGlintOverride},
      {"item_stack_get_attribute_modifiers", HostId::ItemStackGetAttributeModifiers},
      {"item_stack_add_attribute_modifier", HostId::ItemStackAddAttributeModifier},
      {"item_stack_remove_attribute_modifiers", HostId::ItemStackRemoveAttributeModifiers},
      {"item_stack_clear_attribute_modifiers", HostId::ItemStackClearAttributeModifiers},
      {"item_stack_set_custom_data", HostId::ItemStackSetCustomData},
      {"item_stack_get_custom_data", HostId::ItemStackGetCustomData},
      {"item_stack_remove_custom_data", HostId::ItemStackRemoveCustomData},
      {"item_stack_has_custom_data", HostId::ItemStackHasCustomData},
      {"item_stack_get_components", HostId::ItemStackGetComponents},
      {"item_stack_set_component", HostId::ItemStackSetComponent},
      {"item_stack_remove_component", HostId::ItemStackRemoveComponent},
      {"item_stack_get_enchantments", HostId::ItemStackGetEnchantments},
      {"item_stack_add_enchantment", HostId::ItemStackAddEnchantment},
      {"item_stack_remove_enchantment", HostId::ItemStackRemoveEnchantment},
      {"item_stack_get_custom_enchantments", HostId::ItemStackGetCustomEnchantments},
      {"item_stack_add_custom_enchantment", HostId::ItemStackAddCustomEnchantment},
      {"item_stack_remove_custom_enchantment", HostId::ItemStackRemoveCustomEnchantment},
      {"item_stack_get_custom_enchantment_level", HostId::ItemStackGetCustomEnchantmentLevel},
      {"item_stack_has_custom_enchantment", HostId::ItemStackHasCustomEnchantment},
      {"item_stack_get_lore", HostId::ItemStackGetLore},
      {"item_stack_set_lore", HostId::ItemStackSetLore},
      {"item_stack_add_lore", HostId::ItemStackAddLore},
      {"item_stack_get_custom_name", HostId::ItemStackGetCustomName},
      {"item_stack_set_custom_name", HostId::ItemStackSetCustomName},
      {"resource_drop_item", HostId::ResourceDropItem},
      // Unbacked compiled-core imports resolve here. The runtime traps by
      // module/member before consuming arguments; it never returns a fabricated
      // zero result.
      {"component_stub", HostId::Stub},
      {"task_return", HostId::TaskReturn},
      {"task_return_void", HostId::TaskReturnVoid},
      {"server_default_gamemode", HostId::PumpkinServerDefaultGamemode},
      {"server_difficulty", HostId::PumpkinServerDifficulty},
      {"server_has_world", HostId::PumpkinServerHasWorld},
      {"server_max_players", HostId::PumpkinServerMaxPlayers},
      {"server_motd", HostId::PumpkinServerMotd},
      {"server_online_mode", HostId::PumpkinServerOnlineMode},
      {"server_simulation_distance", HostId::PumpkinServerSimulationDistance},
      {"server_view_distance", HostId::PumpkinServerViewDistance},
      {"server_whitelist_enabled", HostId::PumpkinServerWhitelistEnabled},
      {"world_get_block", HostId::WorldGetBlock},
      {"world_get_block_id", HostId::WorldGetBlockId},
      {"world_get_block_info", HostId::WorldGetBlockInfo},
      {"world_set_time", HostId::WorldSetTime},
      // Component-only adapters: the pinned table maps both weather
      // predicates to `world_weather`, and the linker derives the bool here.
      {"world_is_raining", HostId::WorldIsRaining},
      {"world_is_thundering", HostId::WorldIsThundering},
      {"world_set_block", HostId::WorldSetBlock},
      {"player_send_system_message", HostId::PumpkinPlayerSendSystemMessage},
      {"server_broadcast", HostId::PumpkinServerBroadcast},
      {"server_player_by_name", HostId::PumpkinServerPlayer},
      {"text_add_child", HostId::PumpkinTextAddChild},
      {"text_add_text", HostId::PumpkinTextAddText},
      {"text_text", HostId::PumpkinTextText},
      {"text_color_named", HostId::PumpkinTextColorNamed},
      {"text_color_rgb", HostId::PumpkinTextColorRgb},
      {"text_bold", HostId::PumpkinTextBold},
      {"text_italic", HostId::PumpkinTextItalic},
      {"text_underlined", HostId::PumpkinTextUnderlined},
      {"text_strikethrough", HostId::PumpkinTextStrikethrough},
      {"text_obfuscated", HostId::PumpkinTextObfuscated},
      {"text_insertion", HostId::PumpkinTextInsertion},
      {"text_font", HostId::PumpkinTextFont},
      {"text_click_open_url", HostId::PumpkinTextClickOpenUrl},
      {"text_click_open_file", HostId::PumpkinTextClickOpenFile},
      {"text_click_run_command", HostId::PumpkinTextClickRunCommand},
      {"text_click_suggest_command", HostId::PumpkinTextClickSuggestCommand},
      {"text_click_change_page", HostId::PumpkinTextClickChangePage},
      {"text_click_copy_to_clipboard", HostId::PumpkinTextClickCopyToClipboard},
      {"text_hover_show_text", HostId::PumpkinTextHoverShowText},
      {"pumpkin_world_set_block", HostId::PumpkinWorldSetBlock},
      {"pumpkin_world_set_block_async", HostId::PumpkinWorldSetBlockAsync},
      {"pumpkin_world_set_block_spill", HostId::PumpkinWorldSetBlockSpill},
      {"waitable_set_new", HostId::WaitableSetNew},
      {"waitable_set_join", HostId::WaitableSetJoin},
      {"waitable_set_wait", HostId::WaitableSetWait},
      {"world_time", HostId::WorldTime},
      {"world_weather", HostId::WorldWeather},
  };
  const auto it = kIds.find(name);
  if (it == kIds.end())
    return false;
  *out = it->second;
  return true;
}

struct HostImport {
  HostId id = HostId::Log;
  std::string module;
  std::string member;
  std::string name;
  std::uint32_t type_index = 0;
  // A canonical lower whose result is a string, list<string> or two-string
  // record: the call allocates its return area and payload through `realloc`,
  // an absolute core index.
  bool string_result = false;
  bool list_result = false;
  bool record_result = false;
  bool block_result = false;
  bool has_realloc = false;
  std::uint32_t realloc = 0;
  // The component lowered this import with the async ABI. The core call returns
  // a subtask status and the result is delivered through waitable-set.wait.
  bool async_lower = false;
};

// The result an async export hands back through `task.return`. Pumpkin v0.2
// returns `result<_, string>`, so the machine keeps the error text (if any)
// for the caller to report after the core function returns.
struct TaskResult {
  bool called = false;
  bool ok = true;
  std::string error;
};

struct GlobalDef {
  ValType type = ValType::I32;
  bool mut = false;
  Value value;
};

struct TableDef {
  std::uint32_t minimum = 0;
  std::uint32_t maximum = 0;
  bool has_max = false;
};

struct DataSegment {
  std::uint32_t offset = 0;
  std::vector<std::uint8_t> bytes;
  bool passive = false;
};

// An element segment stores 1-based function references: zero is the null
// reference, `n + 1` is function index `n`. That is the same encoding the
// machine uses for a funcref on the operand stack and in a table.
struct ElementSegment {
  std::uint32_t table = 0;
  std::uint32_t offset = 0;
  bool passive = false;
  bool declarative = false;
  std::vector<std::uint32_t> refs;
};

struct ParsedModule {
  std::vector<std::uint8_t> bytes;
  std::vector<FuncType> types;
  std::vector<HostImport> imports;
  std::vector<FuncBody> funcs;
  std::vector<GlobalDef> globals;
  std::vector<TableDef> tables;
  std::vector<ElementSegment> elements;
  std::vector<DataSegment> data;
  std::map<std::string, std::uint32_t> functions_by_export;
  std::uint32_t declared_functions = 0;
  std::uint32_t declared_data = 0;
  bool has_data_count = false;
  bool has_memory = false;
  std::uint32_t memory_min = 0;
  std::uint32_t memory_max = 0;
  bool has_start = false;
  std::uint32_t start = 0;

  std::size_t function_count() const {
    return imports.size() + funcs.size();
  }

  std::size_t declared_function_count() const {
    return imports.size() + declared_functions;
  }
};

std::string signature_text(const FuncType& type) {
  std::string out = "(";
  for (std::size_t i = 0; i < type.params.size(); ++i) {
    if (i != 0)
      out += ",";
    out += valtype_text(type.params[i]);
  }
  out += ")->(";
  if (type.has_result)
    out += valtype_text(type.result);
  return out + ")";
}

// The value type byte the MVP subset hosts, or a reason it is refused.
bool parse_valtype(std::uint8_t byte, ValType* out, std::string* error) {
  switch (byte) {
  case 0x7f:
    *out = ValType::I32;
    return true;
  case 0x7e:
    *out = ValType::I64;
    return true;
  case 0x7d:
    *out = ValType::F32;
    return true;
  case 0x7c:
    *out = ValType::F64;
    return true;
  case 0x7b:
    *error = "SIMD (v128) is not hosted";
    return false;
  case 0x70:
    *out = ValType::FuncRef;
    return true;
  case 0x6f:
    *error = "externref is not hosted (funcref tables are)";
    return false;
  default: {
    char text[64];
    std::snprintf(text, sizeof(text), "unknown value type 0x%02x", byte);
    *error = text;
    return false;
  }
  }
}

bool read_block_type(const std::uint8_t* body, std::size_t size, std::size_t* pc,
                     std::size_t* arity, std::string* error) {
  if (*pc >= size) {
    *error = "a block type runs past the body";
    return false;
  }
  const std::uint8_t byte = body[(*pc)++];
  if (byte == 0x40) {
    *arity = 0;
    return true;
  }
  ValType type{};
  if (!parse_valtype(byte, &type, error))
    return false;
  *arity = 1;
  return true;
}

// Reads one LEB128 immediate without leaving the body.
bool body_uleb(const std::uint8_t* body, std::size_t size, std::size_t* pc, std::uint32_t* out) {
  std::uint32_t result = 0;
  for (unsigned shift = 0; shift < 35; shift += 7) {
    if (*pc >= size)
      return false;
    const std::uint8_t byte = body[(*pc)++];
    if (shift == 28 && (byte & 0xf0) != 0)
      return false;
    result |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
  }
  return false;
}

bool body_sleb(const std::uint8_t* body, std::size_t size, std::size_t* pc, std::uint64_t* out) {
  std::uint64_t result = 0;
  unsigned shift = 0;
  for (;;) {
    if (shift >= 64 || *pc >= size)
      return false;
    const std::uint8_t byte = body[(*pc)++];
    result |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
    shift += 7;
    if ((byte & 0x80) == 0) {
      if (shift < 64 && (byte & 0x40) != 0)
        result |= ~std::uint64_t{0} << shift;
      *out = result;
      return true;
    }
  }
}

bool body_skip(const std::uint8_t* /*body*/, std::size_t size, std::size_t* pc, std::size_t count) {
  if (count > size - *pc)
    return false;
  *pc += count;
  return true;
}

// Walks a body once. This is what makes "refused at load" true for the whole
// feature set the runner does not implement: an opcode outside the subset is
// named here, before the module is instantiated, and the block table it builds
// is what branches use at run time (no scan of the body is ever needed to find
// an `else` or an `end`).
bool validate_body(const std::uint8_t* body, std::size_t size, std::vector<BlockEnds>* blocks,
                   std::string* error) {
  std::vector<std::size_t> open;
  std::size_t pc = 0;
  bool closed = false;
  while (pc < size) {
    if (closed) {
      *error = "trailing bytes after the function's final `end`";
      return false;
    }
    const std::size_t at = pc;
    const std::uint8_t opcode = body[pc++];
    std::uint32_t ignored = 0;
    std::uint64_t ignored64 = 0;
    switch (opcode) {
    case 0x00: // unreachable
    case 0x01: // nop
    case 0x05: // else (handled below)
    case 0x0b: // end (handled below)
    case 0x0f: // return
    case 0x1a: // drop
    case 0x1b: // select
      break;
    case 0x02:
    case 0x03:
    case 0x04: {
      std::size_t arity = 0;
      if (!read_block_type(body, size, &pc, &arity, error))
        return false;
      BlockEnds entry;
      entry.start = static_cast<std::uint32_t>(at);
      entry.body = static_cast<std::uint32_t>(pc);
      entry.kind = opcode == 0x02 ? 0 : (opcode == 0x03 ? 1 : 2);
      entry.arity = arity;
      blocks->push_back(entry);
      open.push_back(blocks->size() - 1);
      break;
    }
    case 0x0c: // br
    case 0x0d: // br_if
    case 0x10: // call
    case 0x20: // local.get
    case 0x21: // local.set
    case 0x22: // local.tee
    case 0x23: // global.get
    case 0x24: // global.set
      if (!body_uleb(body, size, &pc, &ignored)) {
        *error = "an instruction immediate runs past the body";
        return false;
      }
      break;
    case 0x0e: { // br_table
      std::uint32_t count = 0;
      if (!body_uleb(body, size, &pc, &count)) {
        *error = "br_table's target count runs past the body";
        return false;
      }
      for (std::uint32_t i = 0; i <= count; ++i) {
        if (!body_uleb(body, size, &pc, &ignored)) {
          *error = "a br_table target runs past the body";
          return false;
        }
      }
      break;
    }
    case 0x11: { // call_indirect
      std::uint32_t type_index = 0;
      std::uint32_t table_index = 0;
      if (!body_uleb(body, size, &pc, &type_index) || !body_uleb(body, size, &pc, &table_index)) {
        *error = "a call_indirect immediate runs past the body";
        return false;
      }
      break;
    }
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
      *error = "tail calls are not part of the WebAssembly MVP";
      return false;
    case 0x1c: { // typed select
      std::uint32_t count = 0;
      if (!body_uleb(body, size, &pc, &count))
        return false;
      if (count != 1) {
        *error = "typed select must declare exactly one result type";
        return false;
      }
      ValType type{};
      if (pc >= size || !parse_valtype(body[pc++], &type, error)) {
        if (error->empty())
          *error = "typed select has a truncated result type";
        return false;
      }
      break;
    }
    case 0x25: // table.get
    case 0x26: // table.set
      if (!body_uleb(body, size, &pc, &ignored)) {
        *error = "a table immediate runs past the body";
        return false;
      }
      break;
    case 0x28:
    case 0x29:
    case 0x2a:
    case 0x2b:
    case 0x2c:
    case 0x2d:
    case 0x2e:
    case 0x2f:
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x38:
    case 0x39:
    case 0x3a:
    case 0x3b:
    case 0x3c:
    case 0x3d:
    case 0x3e: { // load/store: alignment then offset
      std::uint32_t align = 0;
      std::uint32_t offset = 0;
      if (!body_uleb(body, size, &pc, &align) || !body_uleb(body, size, &pc, &offset)) {
        *error = "a memory immediate runs past the body";
        return false;
      }
      break;
    }
    case 0x3f:
    case 0x40: { // memory.size / memory.grow
      if (pc >= size) {
        *error = "a memory instruction runs past the body";
        return false;
      }
      if (body[pc++] != 0) {
        *error = "multi-memory is not hosted";
        return false;
      }
      break;
    }
    case 0x41: // i32.const
    case 0x42: // i64.const
      if (!body_sleb(body, size, &pc, &ignored64)) {
        *error = "an integer constant runs past the body";
        return false;
      }
      break;
    case 0x43: // f32.const
      if (!body_skip(body, size, &pc, 4)) {
        *error = "an f32 constant runs past the body";
        return false;
      }
      break;
    case 0x44: // f64.const
      if (!body_skip(body, size, &pc, 8)) {
        *error = "an f64 constant runs past the body";
        return false;
      }
      break;
    case 0xd0: { // ref.null
      if (pc >= size) {
        *error = "ref.null runs past the body";
        return false;
      }
      ValType type{};
      if (!parse_valtype(body[pc++], &type, error))
        return false;
      if (type != ValType::FuncRef) {
        *error = "ref.null is not funcref";
        return false;
      }
      break;
    }
    case 0xd1: // ref.is_null
      break;
    case 0xd2: // ref.func
      if (!body_uleb(body, size, &pc, &ignored)) {
        *error = "ref.func runs past the body";
        return false;
      }
      break;
    case 0xfb:
      *error = "garbage collection types are not hosted";
      return false;
    case 0xfc: { // saturating conversions and bulk memory
      std::uint32_t sub = 0;
      if (!body_uleb(body, size, &pc, &sub)) {
        *error = "a 0xfc subopcode runs past the body";
        return false;
      }
      switch (sub) {
      case 0:
      case 1:
      case 2:
      case 3: // i32.trunc_sat_*_s/u
      case 4:
      case 5:
      case 6:
      case 7: // i64.trunc_sat_*_s/u
        break;
      case 8: { // memory.init dataidx memidx
        std::uint32_t data_index = 0;
        std::uint32_t memory_index = 0;
        if (!body_uleb(body, size, &pc, &data_index) ||
            !body_uleb(body, size, &pc, &memory_index)) {
          *error = "memory.init runs past the body";
          return false;
        }
        if (memory_index != 0) {
          *error = "memory.init names a memory other than memory 0";
          return false;
        }
        break;
      }
      case 9: // data.drop dataidx
        if (!body_uleb(body, size, &pc, &ignored)) {
          *error = "data.drop runs past the body";
          return false;
        }
        break;
      case 10: { // memory.copy dstmem srcmem
        std::uint32_t destination = 0;
        std::uint32_t source = 0;
        if (!body_uleb(body, size, &pc, &destination) || !body_uleb(body, size, &pc, &source)) {
          *error = "memory.copy runs past the body";
          return false;
        }
        if (destination != 0 || source != 0) {
          *error = "memory.copy names a memory other than memory 0";
          return false;
        }
        break;
      }
      case 11: // memory.fill memidx
        if (!body_uleb(body, size, &pc, &ignored) || ignored != 0) {
          *error = "memory.fill names a memory other than memory 0";
          return false;
        }
        break;
      case 12:   // table.init elemidx tableidx
      case 14: { // table.copy dsttable srctable
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        if (!body_uleb(body, size, &pc, &left) || !body_uleb(body, size, &pc, &right)) {
          *error = "a table bulk-memory immediate runs past the body";
          return false;
        }
        break;
      }
      case 13: // elem.drop elemidx
      case 15: // table.grow tableidx
      case 16: // table.size tableidx
      case 17: // table.fill tableidx
        if (!body_uleb(body, size, &pc, &ignored)) {
          *error = "a table bulk-memory immediate runs past the body";
          return false;
        }
        break;
      default:
        *error = "0xfc subopcode " + std::to_string(sub) + " is not hosted";
        return false;
      }
      break;
    }
    case 0xfd:
      *error = "unsupported opcode 0xfd (SIMD is not hosted)";
      return false;
    case 0xfe:
      *error = "unsupported opcode 0xfe (threads and atomics are not hosted)";
      return false;
    default:
      if (opcode >= 0x45 && opcode <= 0xc4)
        break;
      {
        char text[96];
        std::snprintf(text, sizeof(text), "unsupported opcode 0x%02x at offset %zu", opcode, at);
        *error = text;
      }
      return false;
    }

    if (opcode == 0x05) { // else
      if (open.empty()) {
        *error = "`else` without a matching `if`";
        return false;
      }
      BlockEnds& block = (*blocks)[open.back()];
      if (block.kind != 2) {
        *error = "`else` outside an `if`";
        return false;
      }
      if (block.has_else) {
        *error = "an `if` has a second `else`";
        return false;
      }
      block.has_else = true;
      block.else_body = static_cast<std::uint32_t>(pc);
    } else if (opcode == 0x0b) { // end
      if (open.empty()) {
        closed = true;
        continue;
      }
      BlockEnds& block = (*blocks)[open.back()];
      block.end = static_cast<std::uint32_t>(pc);
      if (block.kind == 2 && !block.has_else && block.arity != 0) {
        *error = "an `if` with a result type has no `else` branch";
        return false;
      }
      open.pop_back();
    }
  }
  if (!open.empty()) {
    *error = "a block is not closed";
    return false;
  }
  if (!closed) {
    *error = "the function body does not end with `end`";
    return false;
  }
  return true;
}

bool parse_limits(Reader* reader, std::uint32_t* minimum, bool* has_max, std::uint32_t* maximum,
                  std::string* error) {
  std::uint8_t flags = 0;
  if (!reader->u8(&flags)) {
    *error = "a limits descriptor runs past the section";
    return false;
  }
  if (flags > 1) {
    *error = "64-bit or shared memory limits are not hosted";
    return false;
  }
  if (!reader->u32(minimum)) {
    *error = "a memory minimum runs past the section";
    return false;
  }
  *has_max = flags == 1;
  *maximum = 0;
  if (*has_max && !reader->u32(maximum)) {
    *error = "a memory maximum runs past the section";
    return false;
  }
  if (*has_max && *maximum < *minimum) {
    *error = "a memory maximum is below its minimum";
    return false;
  }
  return true;
}

bool parse_table_type(Reader* reader, TableDef* out, std::string* error) {
  std::uint8_t reference = 0;
  if (!reader->u8(&reference)) {
    *error = "a table type runs past the section";
    return false;
  }
  if (reference == 0x6f) {
    *error = "externref tables are not hosted (funcref tables are)";
    return false;
  }
  if (reference != 0x70) {
    char text[64];
    std::snprintf(text, sizeof(text), "unknown table reference type 0x%02x", reference);
    *error = text;
    return false;
  }
  if (!parse_limits(reader, &out->minimum, &out->has_max, &out->maximum, error))
    return false;
  if (out->minimum > kMaxTableEntries) {
    *error = "the module asks for " + std::to_string(out->minimum) +
             " table entries; the budget is " + std::to_string(kMaxTableEntries);
    return false;
  }
  if (out->has_max && out->maximum > kMaxTableEntries) {
    *error = "the table may grow to " + std::to_string(out->maximum) + " entries; the budget is " +
             std::to_string(kMaxTableEntries);
    return false;
  }
  if (!out->has_max)
    out->maximum = kMaxTableEntries;
  return true;
}

// A constant i32 expression used by an active data or element segment. LLVM
// emits i32.const; global.get is accepted for the spec's immutable-global form
// and only where the value is already known at load time.
bool read_i32_expr(Reader* reader, const ParsedModule& module, std::uint32_t* out,
                   std::string* error) {
  std::uint8_t opcode = 0;
  if (!reader->u8(&opcode)) {
    *error = "an offset expression is truncated";
    return false;
  }
  if (opcode == 0x41) {
    std::uint64_t raw = 0;
    if (!reader->s64(&raw)) {
      *error = "an i32.const offset is truncated";
      return false;
    }
    *out = static_cast<std::uint32_t>(raw);
  } else if (opcode == 0x23) {
    std::uint32_t index = 0;
    if (!reader->u32(&index) || index >= module.globals.size()) {
      *error = "an offset names a global that does not exist";
      return false;
    }
    const GlobalDef& global = module.globals[index];
    if (global.mut || global.type != ValType::I32) {
      *error = "an offset global is mutable or not i32";
      return false;
    }
    *out = as_u32(global.value);
  } else {
    *error = "an offset expression is not a constant i32";
    return false;
  }
  std::uint8_t end = 0;
  if (!reader->u8(&end) || end != 0x0b) {
    *error = "an offset expression has no end";
    return false;
  }
  return true;
}

bool read_ref_expr(Reader* reader, std::uint32_t* out, std::string* error) {
  std::uint8_t opcode = 0;
  if (!reader->u8(&opcode)) {
    *error = "a reference expression is truncated";
    return false;
  }
  if (opcode == 0xd2) { // ref.func
    std::uint32_t index = 0;
    if (!reader->u32(&index) || index >= kMaxFunctions) {
      *error = "a ref.func expression names an implausible function";
      return false;
    }
    *out = index + 1;
  } else if (opcode == 0xd0) { // ref.null
    std::uint8_t type = 0;
    if (!reader->u8(&type) || type != 0x70) {
      *error = "a ref.null expression is not funcref";
      return false;
    }
    *out = 0;
  } else {
    *error = "an element expression is neither ref.func nor ref.null";
    return false;
  }
  std::uint8_t end = 0;
  if (!reader->u8(&end) || end != 0x0b) {
    *error = "a reference expression has no end";
    return false;
  }
  return true;
}

bool read_func_indices(Reader* reader, std::vector<std::uint32_t>* out, std::string* error) {
  std::uint32_t count = 0;
  if (!reader->u32(&count) || count > kMaxFunctions) {
    *error = "an element segment has an implausible function count";
    return false;
  }
  out->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t index = 0;
    if (!reader->u32(&index) || index >= kMaxFunctions) {
      *error = "an element segment names an implausible function";
      return false;
    }
    out->push_back(index + 1);
  }
  return true;
}

bool read_ref_exprs(Reader* reader, std::vector<std::uint32_t>* out, std::string* error) {
  std::uint32_t count = 0;
  if (!reader->u32(&count) || count > kMaxFunctions) {
    *error = "an element segment has an implausible expression count";
    return false;
  }
  out->reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t ref = 0;
    if (!read_ref_expr(reader, &ref, error))
      return false;
    out->push_back(ref);
  }
  return true;
}

bool parse_sections(ParsedModule* module, const Limits& limits, std::string* error,
                    const std::vector<CoreImportBinding>* bindings = nullptr) {
  const std::uint8_t* data = module->bytes.data();
  const std::size_t size = module->bytes.size();
  Reader reader(data + 8, size - 8);
  bool seen_start = false;
  std::uint32_t declared_functions = 0;
  bool seen_function_section = false;

  while (reader.remaining() != 0) {
    std::uint8_t id = 0;
    std::uint32_t length = 0;
    if (!reader.u8(&id) || !reader.u32(&length) || length > reader.remaining()) {
      *error = "a section header runs past the file";
      return false;
    }
    Reader payload(nullptr, 0);
    if (!reader.sub(length, &payload)) {
      *error = "a section runs past the file";
      return false;
    }
    reader.advance(length);

    switch (id) {
    case 0: // custom
      break;
    case 1: { // type
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the type section has an implausible count";
        return false;
      }
      module->types.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint8_t form = 0;
        if (!payload.u8(&form) || form != 0x60) {
          *error = "type " + std::to_string(index) + " is not a function type";
          return false;
        }
        FuncType type;
        std::uint32_t params = 0;
        if (!payload.u32(&params) || params > kMaxTypes) {
          *error = "type " + std::to_string(index) + " has an implausible parameter count";
          return false;
        }
        for (std::uint32_t i = 0; i < params; ++i) {
          std::uint8_t byte = 0;
          ValType parsed{};
          if (!payload.u8(&byte) || !parse_valtype(byte, &parsed, error)) {
            if (error->empty())
              *error = "type " + std::to_string(index) + " has a truncated parameter list";
            return false;
          }
          type.params.push_back(parsed);
        }
        std::uint32_t results = 0;
        if (!payload.u32(&results) || results > 1) {
          *error = "type " + std::to_string(index) +
                   (results > 1 ? " has more than one result (multi-value is not hosted)"
                                : " has a truncated result list");
          return false;
        }
        if (results == 1) {
          std::uint8_t byte = 0;
          ValType parsed{};
          if (!payload.u8(&byte) || !parse_valtype(byte, &parsed, error)) {
            if (error->empty())
              *error = "type " + std::to_string(index) + " has a truncated result";
            return false;
          }
          type.has_result = true;
          type.result = parsed;
        }
        module->types.push_back(std::move(type));
      }
      break;
    }
    case 2: { // import
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxFunctions) {
        *error = "the import section has an implausible count";
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        std::string namespace_name;
        std::string name;
        std::uint8_t kind = 0;
        if (!payload.name(&namespace_name) || !payload.name(&name) || !payload.u8(&kind)) {
          *error = "import " + std::to_string(index) + " runs past the section";
          return false;
        }
        if (kind != 0) {
          *error = "import " + namespace_name + "/" + name + " is not a function";
          return false;
        }
        std::uint32_t type_index = 0;
        if (!payload.u32(&type_index) || type_index >= module->types.size()) {
          *error = "import " + namespace_name + "/" + name + " names an undeclared type";
          return false;
        }
        const std::string provided = signature_text(module->types[type_index]);
        HostImport import;
        import.module = namespace_name;
        import.member = name;
        import.type_index = type_index;
        if (bindings != nullptr) {
          // A component's core module imports the functions its `canon lower`
          // definitions produced, under the interface/member names the linker
          // resolved. The linker checked the flattened signature; this table
          // is the runtime's half of that contract.
          const CoreImportBinding* bound = nullptr;
          for (const CoreImportBinding& candidate : *bindings) {
            if (candidate.module == namespace_name && candidate.name == name) {
              bound = &candidate;
              break;
            }
          }
          if (bound == nullptr) {
            *error =
                "import " + namespace_name + "/" + name + " is not bound by the component linker";
            return false;
          }
          if (!host_id(bound->host_function, &import.id)) {
            *error = "the component linker binds " + namespace_name + "/" + name + " to " +
                     bound->host_function + ", which the host does not provide";
            return false;
          }
          if (provided != bound->signature) {
            *error = "import " + namespace_name + "/" + name + " is declared " + provided +
                     " but the component linker binds " + bound->signature;
            return false;
          }
          import.name = bound->host_function;
          import.string_result = bound->string_result;
          import.list_result = bound->list_result;
          import.record_result = bound->record_result;
          import.block_result = bound->block_result;
          import.has_realloc = bound->has_realloc;
          import.realloc = bound->realloc;
          import.async_lower = bound->async_lower;
          if (import.id == HostId::ConfigGetString && !import.string_result) {
            *error = namespace_name + "/" + name +
                     " is bound to config_get_string without a string result";
            return false;
          }
          if (import.id == HostId::ConfigKeys && !import.list_result) {
            *error = namespace_name + "/" + name + " is bound to config_keys without a list result";
            return false;
          }
          if (import.id == HostId::ConfigPair && !import.record_result) {
            *error =
                namespace_name + "/" + name + " is bound to config_pair without a record result";
            return false;
          }
          if (import.id == HostId::WorldGetBlockInfo && !import.block_result) {
            *error = namespace_name + "/" + name +
                     " is bound to world_get_block_info without a block result";
            return false;
          }
          module->imports.push_back(std::move(import));
          continue;
        }
        if (namespace_name != kHostNamespace) {
          *error = "import " + namespace_name + "/" + name + " is outside the pinned " +
                   kHostNamespace + " surface";
          return false;
        }
        if (!host_id(name, &import.id)) {
          *error = "import " + namespace_name + "/" + name + " is not provided by the host";
          return false;
        }
        const HostFunctionRef* entry = nullptr;
        for (const HostFunctionRef& candidate : host_surface()) {
          if (name == candidate.name)
            entry = &candidate;
        }
        if (entry == nullptr) {
          *error = "import " + name + " is not provided by the host";
          return false;
        }
        if (provided != entry->signature) {
          *error = "import " + name + " is declared " + provided + " but the host provides " +
                   entry->signature;
          return false;
        }
        import.name = name;
        if (import.id == HostId::ConfigGetString) {
          *error = "import " + namespace_name + "/" + name +
                   " returns a string and needs a canon lower with a realloc option";
          return false;
        }
        if (import.id == HostId::ConfigKeys) {
          *error = "import " + namespace_name + "/" + name +
                   " returns a list and needs a canon lower with a realloc option";
          return false;
        }
        if (import.id == HostId::ConfigPair) {
          *error = "import " + namespace_name + "/" + name +
                   " returns a record and needs a canon lower with a realloc option";
          return false;
        }
        if (import.id == HostId::WorldGetBlockInfo) {
          *error = "import " + namespace_name + "/" + name +
                   " returns a block record and needs a canon lower with a realloc option";
          return false;
        }
        module->imports.push_back(std::move(import));
      }
      break;
    }
    case 3: { // function
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxFunctions) {
        *error = "the function section has an implausible count";
        return false;
      }
      declared_functions = count;
      module->declared_functions = count;
      seen_function_section = true;
      break;
    }
    case 4: { // table
      std::uint32_t count = 0;
      if (!payload.u32(&count)) {
        *error = "the table section has no count";
        return false;
      }
      if (count > 8) {
        *error =
            "the module declares " + std::to_string(count) + " tables; at most eight are hosted";
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        TableDef table;
        if (!parse_table_type(&payload, &table, error)) {
          *error = "table " + std::to_string(index) + ": " + *error;
          return false;
        }
        module->tables.push_back(table);
      }
      break;
    }
    case 5: { // memory
      std::uint32_t count = 0;
      if (!payload.u32(&count)) {
        *error = "the memory section has no count";
        return false;
      }
      if (count > 1) {
        *error = "the module declares " + std::to_string(count) +
                 " memories; multi-memory is not hosted";
        return false;
      }
      if (count == 1) {
        bool has_max = false;
        if (!parse_limits(&payload, &module->memory_min, &has_max, &module->memory_max, error))
          return false;
        module->has_memory = true;
        if (!has_max)
          module->memory_max = limits.max_memory_pages;
        if (module->memory_min > limits.max_memory_pages) {
          *error = "the module asks for " + std::to_string(module->memory_min) +
                   " memory pages; the budget is " + std::to_string(limits.max_memory_pages);
          return false;
        }
        if (module->memory_max > limits.max_memory_pages) {
          *error = "the module may grow to " + std::to_string(module->memory_max) +
                   " memory pages; the budget is " + std::to_string(limits.max_memory_pages);
          return false;
        }
      }
      break;
    }
    case 6: { // global
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the global section has an implausible count";
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint8_t type_byte = 0;
        std::uint8_t mutability = 0;
        GlobalDef global;
        if (!payload.u8(&type_byte) || !parse_valtype(type_byte, &global.type, error) ||
            !payload.u8(&mutability) || mutability > 1) {
          if (error->empty())
            *error = "global " + std::to_string(index) + " has a malformed type";
          return false;
        }
        global.mut = mutability == 1;
        std::uint8_t opcode = 0;
        if (!payload.u8(&opcode)) {
          *error = "global " + std::to_string(index) + " has no initializer";
          return false;
        }
        std::uint64_t raw = 0;
        std::uint32_t global_index = 0;
        switch (opcode) {
        case 0x41: { // i32.const
          std::uint32_t value = 0;
          std::uint8_t byte = 0;
          std::uint32_t shift = 0;
          bool done = false;
          while (shift < 35) {
            if (!payload.u8(&byte))
              break;
            value |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) {
              done = true;
              break;
            }
          }
          if (!done) {
            *error = "global " + std::to_string(index) + " has a truncated i32 initializer";
            return false;
          }
          if (global.type != ValType::I32) {
            *error = "global " + std::to_string(index) + " is typed " + valtype_text(global.type) +
                     " but has an i32 initializer";
            return false;
          }
          global.value = make_i32(value);
          break;
        }
        case 0x42: {
          std::uint8_t byte = 0;
          std::uint32_t shift = 0;
          bool done = false;
          while (shift < 70) {
            if (!payload.u8(&byte))
              break;
            raw |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) {
              if (shift < 64 && (byte & 0x40) != 0)
                raw |= ~std::uint64_t{0} << shift;
              done = true;
              break;
            }
          }
          if (!done || global.type != ValType::I64) {
            *error = "global " + std::to_string(index) + " has a malformed i64 initializer";
            return false;
          }
          global.value = make_i64(raw);
          break;
        }
        case 0x43: {
          const std::uint8_t* raw_bytes = nullptr;
          if (!payload.bytes(4, &raw_bytes) || global.type != ValType::F32) {
            *error = "global " + std::to_string(index) + " has a malformed f32 initializer";
            return false;
          }
          std::uint32_t bits = 0;
          std::memcpy(&bits, raw_bytes, 4);
          global.value = Value{ValType::F32, bits};
          break;
        }
        case 0x44: {
          const std::uint8_t* raw_bytes = nullptr;
          if (!payload.bytes(8, &raw_bytes) || global.type != ValType::F64) {
            *error = "global " + std::to_string(index) + " has a malformed f64 initializer";
            return false;
          }
          std::uint64_t bits = 0;
          std::memcpy(&bits, raw_bytes, 8);
          global.value = Value{ValType::F64, bits};
          break;
        }
        case 0xd0: { // ref.null funcref
          std::uint8_t reference = 0;
          if (!payload.u8(&reference) || reference != 0x70 || global.type != ValType::FuncRef) {
            *error = "global " + std::to_string(index) + " has a malformed ref.null initializer";
            return false;
          }
          global.value = Value{ValType::FuncRef, 0};
          break;
        }
        case 0xd2: { // ref.func
          std::uint32_t function = 0;
          if (!payload.u32(&function) || function >= module->declared_function_count()) {
            *error = "global " + std::to_string(index) + " names a function that does not exist";
            return false;
          }
          if (global.type != ValType::FuncRef) {
            *error = "global " + std::to_string(index) + " has a funcref initializer but is " +
                     valtype_text(global.type);
            return false;
          }
          global.value = Value{ValType::FuncRef, static_cast<std::uint64_t>(function) + 1};
          break;
        }
        case 0x23: { // global.get of an earlier immutable global
          if (!payload.u32(&global_index) || global_index >= module->globals.size()) {
            *error = "global " + std::to_string(index) + " references a global that does not exist";
            return false;
          }
          const GlobalDef& source = module->globals[global_index];
          if (source.mut || source.type != global.type) {
            *error =
                "global " + std::to_string(index) + " copies a mutable or differently typed global";
            return false;
          }
          global.value = source.value;
          break;
        }
        default:
          *error = "global " + std::to_string(index) + " uses an unsupported initializer";
          return false;
        }
        std::uint8_t end = 0;
        if (!payload.u8(&end) || end != 0x0b) {
          *error = "global " + std::to_string(index) + " has no initializer end";
          return false;
        }
        module->globals.push_back(global);
      }
      break;
    }
    case 7: { // export
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxFunctions) {
        *error = "the export section has an implausible count";
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        std::string name;
        std::uint8_t kind = 0;
        std::uint32_t target = 0;
        if (!payload.name(&name) || !payload.u8(&kind) || !payload.u32(&target)) {
          *error = "export " + std::to_string(index) + " runs past the section";
          return false;
        }
        if (kind != 0)
          continue; // a memory or global export is not a lifecycle entry point
        if (!module->functions_by_export.emplace(name, target).second) {
          *error = "the export \"" + name + "\" is declared twice";
          return false;
        }
      }
      break;
    }
    case 8: { // start
      if (!payload.u32(&module->start)) {
        *error = "the start section has no function index";
        return false;
      }
      module->has_start = true;
      seen_start = true;
      break;
    }
    case 9: { // element
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the element section has an implausible count";
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t flags = 0;
        if (!payload.u32(&flags) || flags > 7) {
          *error = "element segment " + std::to_string(index) + " has an unsupported flags value";
          return false;
        }
        ElementSegment segment;
        segment.passive = flags == 1 || flags == 5;
        segment.declarative = flags == 3 || flags == 7;
        if (flags == 0 || flags == 4) {
          segment.table = 0;
          if (!read_i32_expr(&payload, *module, &segment.offset, error))
            return false;
        } else if (flags == 2 || flags == 6) {
          if (!payload.u32(&segment.table) || segment.table >= module->tables.size()) {
            *error =
                "element segment " + std::to_string(index) + " names a table that does not exist";
            return false;
          }
          if (!read_i32_expr(&payload, *module, &segment.offset, error))
            return false;
        }
        if (flags == 1 || flags == 2 || flags == 3) {
          std::uint8_t elemkind = 0;
          if (!payload.u8(&elemkind) || elemkind != 0x00) {
            *error = "element segment " + std::to_string(index) + " is not a funcref segment";
            return false;
          }
        } else if (flags == 5 || flags == 6 || flags == 7) {
          std::uint8_t reference = 0;
          if (!payload.u8(&reference) || reference != 0x70) {
            *error = "element segment " + std::to_string(index) + " is not a funcref segment";
            return false;
          }
        }
        if (flags == 0 || flags == 1 || flags == 2 || flags == 3) {
          if (!read_func_indices(&payload, &segment.refs, error))
            return false;
        } else {
          if (!read_ref_exprs(&payload, &segment.refs, error))
            return false;
        }
        for (std::uint32_t ref : segment.refs) {
          if (ref != 0 && ref - 1 >= module->declared_function_count()) {
            *error = "element segment " + std::to_string(index) +
                     " names a function that does not exist";
            return false;
          }
        }
        module->elements.push_back(std::move(segment));
      }
      break;
    }
    case 10: { // code
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxFunctions) {
        *error = "the code section has an implausible count";
        return false;
      }
      if (count != 0 && !seen_function_section) {
        *error = "the code section has bodies but there is no function section";
        return false;
      }
      if (seen_function_section && count != declared_functions) {
        *error = "the function section declares " + std::to_string(declared_functions) +
                 " functions but the code section has " + std::to_string(count);
        return false;
      }
      module->funcs.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t body_size = 0;
        Reader body_reader(nullptr, 0);
        if (!payload.u32(&body_size) || !payload.sub(body_size, &body_reader)) {
          *error = "code body " + std::to_string(index) + " runs past the section";
          return false;
        }
        payload.advance(body_size);

        FuncBody function;
        std::uint32_t groups = 0;
        if (!body_reader.u32(&groups) || groups > kMaxLocals) {
          *error = "code body " + std::to_string(index) + " has an implausible local count";
          return false;
        }
        for (std::uint32_t group = 0; group < groups; ++group) {
          std::uint32_t repeats = 0;
          std::uint8_t type_byte = 0;
          ValType type{};
          if (!body_reader.u32(&repeats) || !body_reader.u8(&type_byte) ||
              !parse_valtype(type_byte, &type, error)) {
            if (error->empty())
              *error = "code body " + std::to_string(index) + " has a malformed local declaration";
            return false;
          }
          if (repeats > kMaxLocals || function.locals.size() + repeats > kMaxLocals) {
            *error = "code body " + std::to_string(index) + " declares too many locals";
            return false;
          }
          function.locals.insert(function.locals.end(), repeats, type);
        }
        // `body_reader` is a sub-reader, so its position is relative to the
        // body start while the executor needs an offset into the whole file.
        // Keep both: the absolute offset for `frame.body` and the relative one
        // for validation (whose pointer already starts at the body).
        const std::size_t body_relative = body_reader.position();
        function.body_offset =
            static_cast<std::size_t>(body_reader.data() - module->bytes.data()) + body_relative;
        function.body_size = body_reader.remaining();
        function.type_index = 0;
        if (!validate_body(body_reader.data() + body_relative, function.body_size, &function.blocks,
                           error)) {
          *error = "code body " + std::to_string(index) + ": " + *error;
          return false;
        }
        module->funcs.push_back(std::move(function));
      }
      break;
    }
    case 11: { // data
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the data section has an implausible count";
        return false;
      }
      if (module->has_data_count && count != module->declared_data) {
        *error = "the data count section declares " + std::to_string(module->declared_data) +
                 " segments but the data section has " + std::to_string(count);
        return false;
      }
      for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t flags = 0;
        if (!payload.u32(&flags) || flags > 2) {
          *error = "data segment " + std::to_string(index) + " has an unsupported flags value";
          return false;
        }
        DataSegment segment;
        segment.passive = flags == 1;
        if (flags == 2) {
          std::uint32_t memory = 0;
          if (!payload.u32(&memory) || memory != 0) {
            *error =
                "data segment " + std::to_string(index) + " names a memory other than memory 0";
            return false;
          }
        }
        if (!segment.passive && !read_i32_expr(&payload, *module, &segment.offset, error)) {
          *error = "data segment " + std::to_string(index) + ": " + *error;
          return false;
        }
        std::uint32_t size = 0;
        const std::uint8_t* bytes = nullptr;
        if (!payload.u32(&size) || !payload.bytes(size, &bytes)) {
          *error = "data segment " + std::to_string(index) + " runs past the section";
          return false;
        }
        segment.bytes.assign(bytes, bytes + size);
        module->data.push_back(std::move(segment));
      }
      break;
    }
    case 12: { // data count
      std::uint32_t count = 0;
      if (!payload.u32(&count)) {
        *error = "the data count section has no count";
        return false;
      }
      module->has_data_count = true;
      module->declared_data = count;
      break;
    }
    case 13: { // tag
      std::uint32_t count = 0;
      if (!payload.u32(&count)) {
        *error = "the tag section has no count";
        return false;
      }
      if (count != 0) {
        *error = "the module uses exception tags; exceptions are not hosted";
        return false;
      }
      break;
    }
    default: {
      char text[64];
      std::snprintf(text, sizeof(text), "unknown section id %u", id);
      *error = text;
      return false;
    }
    }
    (void)seen_start;
  }

  // The function section names each body's type; fill them in now that both
  // sections have been read.
  if (seen_function_section) {
    // Re-read the function section's type indices. The gate has already
    // checked the section order, so it is somewhere before the code section;
    // parsing it a second time is cheaper than holding a second copy.
    Reader again(data + 8, size - 8);
    while (again.remaining() != 0) {
      std::uint8_t id = 0;
      std::uint32_t length = 0;
      if (!again.u8(&id) || !again.u32(&length) || length > again.remaining())
        break;
      Reader payload(nullptr, 0);
      if (!again.sub(length, &payload))
        break;
      again.advance(length);
      if (id != 3)
        continue;
      std::uint32_t count = 0;
      if (!payload.u32(&count))
        break;
      for (std::uint32_t index = 0; index < count && index < module->funcs.size(); ++index) {
        std::uint32_t type_index = 0;
        if (!payload.u32(&type_index))
          break;
        if (type_index >= module->types.size()) {
          *error = "function " + std::to_string(index) + " names an undeclared type";
          return false;
        }
        module->funcs[index].type_index = type_index;
      }
      break;
    }
    if (module->funcs.size() != declared_functions) {
      *error = "the function section declares " + std::to_string(declared_functions) +
               " functions but " + std::to_string(module->funcs.size()) + " bodies were read";
      return false;
    }
  }

  if (module->has_data_count && module->declared_data != module->data.size()) {
    *error = "the data count section declares " + std::to_string(module->declared_data) +
             " segments but the data section has " + std::to_string(module->data.size());
    return false;
  }
  for (const auto& entry : module->functions_by_export) {
    if (entry.second >= module->function_count()) {
      *error = "the export \"" + entry.first + "\" names a function that does not exist";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------

struct Label {
  bool is_loop = false;
  std::size_t height = 0;
  std::size_t branch_arity = 0;
  std::size_t end_arity = 0;
  std::size_t target = 0;
};

struct Frame {
  const FuncBody* function = nullptr;
  const FuncType* type = nullptr;
  const std::uint8_t* body = nullptr;
  std::size_t size = 0;
  std::size_t pc = 0;
  std::size_t stack_base = 0;
  std::vector<Value> locals;
  std::vector<Label> labels;
};

// A table entry is a 1-based function index, exactly like a funcref value;
// zero means null.
struct Table {
  std::vector<std::uint32_t> entries;
  std::uint32_t maximum = 0;
};

bool same_func_type(const FuncType& left, const FuncType& right) {
  return left.params == right.params && left.has_result == right.has_result &&
         (!left.has_result || left.result == right.result);
}

class Machine {
public:
  Machine(const ParsedModule& module, const Limits& limits, Host& host, RunReport* report)
      : module_(module), limits_(limits), host_(host), report_(report) {}

  void set_deadline(std::chrono::steady_clock::time_point deadline) {
    deadline_ = deadline;
  }

  void instantiate() {
    if (module_.has_memory) {
      const std::size_t pages = module_.memory_min;
      memory_.assign(pages * kPageBytes, 0);
      max_pages_ = module_.memory_max;
      report_->peak_pages = static_cast<std::uint32_t>(pages);
    }
    tables_.clear();
    tables_.reserve(module_.tables.size());
    for (const TableDef& definition : module_.tables) {
      Table table;
      table.entries.assign(definition.minimum, 0);
      table.maximum = definition.has_max ? definition.maximum : kMaxTableEntries;
      tables_.push_back(std::move(table));
    }
    globals_ = module_.globals;
    element_dropped_.assign(module_.elements.size(), 0);
    for (std::size_t index = 0; index < module_.elements.size(); ++index) {
      const ElementSegment& segment = module_.elements[index];
      if (segment.declarative) {
        element_dropped_[index] = 1;
        continue;
      }
      if (segment.passive)
        continue;
      if (segment.table >= tables_.size())
        throw Stop{"element segment names a table that does not exist", StopKind::Refuse};
      Table& table = tables_[segment.table];
      const std::uint64_t end = static_cast<std::uint64_t>(segment.offset) + segment.refs.size();
      if (end > table.entries.size())
        throw Stop{"element segment is outside the module's table", StopKind::Refuse};
      std::copy(segment.refs.begin(), segment.refs.end(), table.entries.begin() + segment.offset);
      element_dropped_[index] = 1;
    }
    data_dropped_.assign(module_.data.size(), 0);
    for (std::size_t index = 0; index < module_.data.size(); ++index) {
      const DataSegment& segment = module_.data[index];
      if (segment.passive)
        continue;
      const std::uint64_t end = static_cast<std::uint64_t>(segment.offset) + segment.bytes.size();
      if (end > memory_.size()) {
        throw Stop{"data segment is outside the module's memory", StopKind::Refuse};
      }
      if (!segment.bytes.empty())
        std::memcpy(memory_.data() + segment.offset, segment.bytes.data(), segment.bytes.size());
      data_dropped_[index] = 1;
    }
    if (module_.has_start)
      call_lifecycle(module_.start, false);
  }

  std::int32_t call_lifecycle(std::uint32_t function_index, bool with_argument,
                              std::uint32_t argument = 1) {
    stack_.clear();
    frames_.clear();
    steps_ = 0;
    if (with_argument)
      stack_.push_back(make_i32(argument));
    call_index(function_index);
    try {
      execute();
    } catch (...) {
      report_->steps += steps_;
      throw;
    }
    report_->steps += steps_;
    if (stack_.empty())
      return 0;
    const std::int32_t result = as_i32(stack_.back());
    stack_.clear();
    return result;
  }

  // `init-plugin` is synchronous, takes nothing and has no result.
  void call_void(std::uint32_t function_index) {
    stack_.clear();
    frames_.clear();
    steps_ = 0;
    call_index(function_index);
    try {
      execute();
    } catch (...) {
      report_->steps += steps_;
      throw;
    }
    report_->steps += steps_;
    stack_.clear();
  }

  // A stackful async lift: the core function takes the flattened parameters,
  // returns no core result, and must call `task.return` before its last thread
  // exits. The caller only supports the synchronous-completion case, which is
  // what a Pumpkin on-load/on-unload with no awaited host call produces.
  bool call_async(std::uint32_t function_index, bool with_handle, std::string* error,
                  std::uint32_t argument = 1) {
    stack_.clear();
    frames_.clear();
    steps_ = 0;
    task_ = TaskResult{};
    if (with_handle)
      stack_.push_back(make_i32(argument));
    call_index(function_index);
    try {
      execute();
    } catch (...) {
      report_->steps += steps_;
      throw;
    }
    report_->steps += steps_;
    if (!task_.called) {
      *error = "the async export returned without calling task.return";
      return false;
    }
    if (!task_.ok) {
      *error = task_.error;
      return false;
    }
    return true;
  }

  // Calls a compiled `handle-event` for a tick-start event. The canonical ABI
  // flattening of the tick-start variant is one discriminant i32 plus the
  // event's single s32 payload; the core function returns a pointer to an
  // 8-byte return area. A different returned case is a handler error, as the
  // WIT contract requires the host to keep the original event.
  bool call_event(std::uint32_t function_index, std::uint32_t event_id, std::uint32_t server_handle,
                  std::uint32_t event_case, std::uint32_t tick, std::uint32_t* returned_case,
                  std::uint32_t* returned_tick, std::string* error) {
    stack_.clear();
    frames_.clear();
    steps_ = 0;
    stack_.push_back(make_i32(event_id));
    stack_.push_back(make_i32(server_handle));
    stack_.push_back(make_i32(event_case));
    stack_.push_back(make_i32(tick));
    call_index(function_index);
    try {
      execute();
    } catch (...) {
      report_->steps += steps_;
      throw;
    }
    report_->steps += steps_;
    if (stack_.empty()) {
      *error = "handle-event returned no event";
      return false;
    }
    const std::uint32_t area = as_u32(stack_.back());
    stack_.clear();
    const std::uint8_t* bytes = memory_at(area, 8);
    const std::uint32_t result_case = bytes[0];
    std::uint32_t result_tick = 0;
    std::memcpy(&result_tick, bytes + 4, sizeof(result_tick));
    *returned_case = result_case;
    *returned_tick = result_tick;
    if (result_case != event_case || result_tick != tick) {
      *error = "handle-event returned event case " + std::to_string(result_case) + " with tick " +
               std::to_string(result_tick) + ", expected case " + std::to_string(event_case) +
               " with tick " + std::to_string(tick);
      return false;
    }
    record_host_call("handle_event", static_cast<std::int64_t>(tick));
    return true;
  }

  bool event_registered(std::uint32_t event_type) const {
    return event_handlers_.find(event_type) != event_handlers_.end();
  }

  std::uint32_t new_context() {
    return new_resource(ResourceKind::Context);
  }
  std::uint32_t new_server() {
    return new_resource(ResourceKind::Server);
  }
  std::uint32_t new_world(std::string name) {
    const std::uint32_t handle = new_resource(ResourceKind::World);
    world_names_[handle] = std::move(name);
    return handle;
  }
  std::uint32_t new_player(std::string name) {
    const std::uint32_t handle = new_resource(ResourceKind::Player);
    player_names_[handle] = std::move(name);
    return handle;
  }
  std::uint32_t new_text(TextComponent text) {
    const std::uint32_t handle = new_resource(ResourceKind::Text);
    text_values_[handle] = std::move(text);
    return handle;
  }
  struct ItemAttributeModifierValue {
    std::uint32_t attribute = 0;
    std::string id;
    double amount = 0;
    std::uint32_t operation = 0;
    std::uint32_t slot = 0;
  };

  // Declared before the method that takes it by value: a member type must be
  // complete at the point of use.
  struct ItemStackValue {
    std::string registry_key;
    std::uint8_t count = 0;
    std::int32_t max_count = 0;
    std::int32_t damage = 0;
    std::int32_t max_damage = 0;
    bool damageable = false;
    bool unbreakable = false;
    std::int32_t repair_cost = 0;
    std::optional<std::string> item_model;
    std::optional<bool> glint_override;
    std::vector<ItemAttributeModifierValue> attribute_modifiers;
    std::map<std::string, std::map<std::string, nbt::Value>> custom_data;
    std::map<std::uint32_t, std::vector<std::uint8_t>> components;
    std::map<std::uint32_t, std::uint32_t> enchantments;
    std::map<std::string, std::uint32_t> custom_enchantments;
    std::vector<TextComponent> lore;
    std::optional<TextComponent> custom_name;
  };

  std::uint32_t new_item(ItemStackValue value) {
    const std::uint32_t handle = new_resource(ResourceKind::Item);
    item_values_[handle] = std::move(value);
    return handle;
  }

  struct CommandValue {
    std::vector<std::string> names;
    std::string description;
    std::uint32_t handler_id = 0;
    bool has_handler = false;
    std::vector<std::uint32_t> nodes;
  };

  std::uint32_t new_command(CommandValue value) {
    const std::uint32_t handle = new_resource(ResourceKind::Command);
    command_values_[handle] = std::move(value);
    return handle;
  }

  struct CommandNodeValue {
    std::string name;
    bool is_argument = false;
    CommandArgument argument;
    std::vector<std::uint32_t> children;
    std::uint32_t execute_handler = 0;
    std::uint32_t suggest_handler = 0;
    std::uint32_t require_handler = 0;
    bool has_execute = false;
    bool has_suggest = false;
    bool has_require = false;
  };

  std::uint32_t new_command_node(CommandNodeValue value) {
    const std::uint32_t handle = new_resource(ResourceKind::CommandNode);
    command_node_values_[handle] = std::move(value);
    return handle;
  }

  void set_task_callback(bool enabled, std::uint32_t function_index, bool returns_status,
                         bool async) {
    task_callback_enabled_ = enabled;
    task_callback_ = function_index;
    task_callback_status_ = returns_status;
    task_callback_async_ = async;
  }

  std::uint32_t schedule_task(std::uint32_t handler_id, std::uint64_t delay_ticks,
                              std::uint64_t period_ticks, bool repeating) {
    if (!task_callback_enabled_)
      return 0;
    const std::uint32_t id = next_scheduled_task_++;
    scheduled_tasks_[id] =
        ScheduledTask{handler_id, current_tick_ + delay_ticks, period_ticks, repeating};
    return id;
  }

  void cancel_task(std::uint32_t id) {
    scheduled_tasks_.erase(id);
  }

  // Runs the tasks due at `tick` in task-id order. A repeating task is
  // rescheduled from the current tick after its callback; a one-shot is
  // removed before the call so it cannot run twice.
  bool run_due_tasks(std::uint64_t tick, std::string* error) {
    current_tick_ = tick;
    if (!task_callback_enabled_)
      return true;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> due;
    for (auto it = scheduled_tasks_.begin(); it != scheduled_tasks_.end();) {
      if (it->second.due > tick) {
        ++it;
        continue;
      }
      due.emplace_back(it->first, it->second.handler_id);
      if (it->second.repeating) {
        it->second.due = tick + it->second.period;
        ++it;
      } else {
        it = scheduled_tasks_.erase(it);
      }
    }
    for (const auto& entry : due) {
      const std::uint32_t server = new_server();
      const bool ok = call_task(entry.second, server, error);
      drop_resource(server, ResourceKind::Server);
      if (!ok)
        return false;
    }
    return true;
  }

private:
  enum class ResourceKind { Context, Server, World, Player, Text, Item, Command, CommandNode };

  struct ScheduledTask {
    std::uint32_t handler_id = 0;
    std::uint64_t due = 0;
    std::uint64_t period = 0;
    bool repeating = false;
  };

  std::uint32_t new_resource(ResourceKind kind) {
    const std::uint32_t handle = next_resource_++;
    resources_[handle] = kind;
    return handle;
  }

  bool valid_resource(std::uint32_t handle, ResourceKind kind) const {
    const auto it = resources_.find(handle);
    return it != resources_.end() && it->second == kind;
  }

  CommandValue& command_at(std::uint32_t handle, const char* member) {
    if (!valid_resource(handle, ResourceKind::Command))
      trap(std::string(member) + " used an invalid command handle " + std::to_string(handle));
    const auto command = command_values_.find(handle);
    if (command == command_values_.end())
      trap(std::string(member) + " used a stale command handle");
    return command->second;
  }

  CommandNodeValue& command_node_at(std::uint32_t handle, const char* member) {
    if (!valid_resource(handle, ResourceKind::CommandNode))
      trap(std::string(member) + " used an invalid command-node handle " + std::to_string(handle));
    const auto node = command_node_values_.find(handle);
    if (node == command_node_values_.end())
      trap(std::string(member) + " used a stale command-node handle");
    return node->second;
  }

  // Resolves the live node handles into a value tree at registration time. A
  // malformed tree is refused rather than partially published: a cycle, a stale
  // handle or an unbounded branch would otherwise reach the host as a command
  // the plugin never built.
  CommandNodeRegistration command_node_registration(std::uint32_t handle,
                                                    std::vector<std::uint32_t>* ancestry,
                                                    std::uint32_t* count, unsigned int depth) {
    if (depth > 64)
      trap("context.register-command received a command tree deeper than 64 nodes");
    if (*count >= 4096)
      trap("context.register-command received more than 4096 command nodes");
    if (std::find(ancestry->begin(), ancestry->end(), handle) != ancestry->end())
      trap("context.register-command received a cyclic command tree");
    CommandNodeValue& value = command_node_at(handle, "context.register-command");
    CommandNodeRegistration node;
    node.kind = value.is_argument ? CommandNodeRegistration::Kind::Argument
                                  : CommandNodeRegistration::Kind::Literal;
    node.name = value.name;
    node.argument = value.argument;
    node.executable = value.has_execute;
    if (value.has_execute)
      node.execute_handler_id = value.execute_handler;
    if (value.has_suggest)
      node.suggest_handler_id = value.suggest_handler;
    if (value.has_require)
      node.require_handler_id = value.require_handler;
    ancestry->push_back(handle);
    for (const std::uint32_t child : value.children)
      node.children.push_back(command_node_registration(child, ancestry, count, depth + 1));
    ancestry->pop_back();
    ++*count;
    return node;
  }

  TextComponent& text_at(std::uint32_t handle, const char* member) {
    if (!valid_resource(handle, ResourceKind::Text))
      trap(std::string(member) + " used an invalid text handle " + std::to_string(handle));
    const auto text = text_values_.find(handle);
    if (text == text_values_.end())
      trap(std::string(member) + " used a stale text handle");
    return text->second;
  }

  ItemStackValue& item_at(std::uint32_t handle, const char* member) {
    if (!valid_resource(handle, ResourceKind::Item))
      trap(std::string(member) + " used an invalid item handle " + std::to_string(handle));
    const auto item = item_values_.find(handle);
    if (item == item_values_.end())
      trap(std::string(member) + " used a stale item handle");
    return item->second;
  }

  static bool same_text(const TextComponent& left, const TextComponent& right) {
    if (left.text != right.text || left.color != right.color || left.has_bold != right.has_bold ||
        left.bold != right.bold || left.has_italic != right.has_italic ||
        left.italic != right.italic || left.has_underlined != right.has_underlined ||
        left.underlined != right.underlined || left.has_strikethrough != right.has_strikethrough ||
        left.strikethrough != right.strikethrough || left.has_obfuscated != right.has_obfuscated ||
        left.obfuscated != right.obfuscated || left.insertion != right.insertion ||
        left.font != right.font || left.click_action != right.click_action ||
        left.click_value != right.click_value)
      return false;
    if (static_cast<bool>(left.hover_text) != static_cast<bool>(right.hover_text))
      return false;
    if (left.hover_text && !same_text(*left.hover_text, *right.hover_text))
      return false;
    if (left.children.size() != right.children.size())
      return false;
    for (std::size_t i = 0; i < left.children.size(); ++i) {
      if (!same_text(left.children[i], right.children[i]))
        return false;
    }
    return true;
  }

  static bool same_nbt(const nbt::Value& left, const nbt::Value& right) {
    if (left.type != right.type)
      return false;
    switch (left.type) {
    case nbt::Tag::End:
      return true;
    case nbt::Tag::Byte:
    case nbt::Tag::Short:
    case nbt::Tag::Int:
    case nbt::Tag::Long:
      return left.i == right.i;
    case nbt::Tag::Float:
    case nbt::Tag::Double:
      return left.d == right.d;
    case nbt::Tag::String:
      return left.s == right.s;
    case nbt::Tag::ByteArray:
      return left.bytes == right.bytes;
    case nbt::Tag::IntArray:
      return left.ints == right.ints;
    case nbt::Tag::LongArray:
      return left.longs == right.longs;
    case nbt::Tag::List:
      if (left.list_type != right.list_type || left.items.size() != right.items.size())
        return false;
      for (std::size_t i = 0; i < left.items.size(); ++i) {
        if (!same_nbt(left.items[i], right.items[i]))
          return false;
      }
      return true;
    case nbt::Tag::Compound:
      if (left.fields.size() != right.fields.size())
        return false;
      for (std::size_t i = 0; i < left.fields.size(); ++i) {
        if (left.fields[i].first != right.fields[i].first ||
            !same_nbt(left.fields[i].second, right.fields[i].second))
          return false;
      }
      return true;
    }
    return false;
  }

  static bool same_item_data(const ItemStackValue& left, const ItemStackValue& right) {
    if (left.registry_key != right.registry_key || left.damage != right.damage ||
        left.unbreakable != right.unbreakable || left.repair_cost != right.repair_cost ||
        left.item_model != right.item_model || left.glint_override != right.glint_override ||
        left.enchantments != right.enchantments ||
        left.custom_enchantments != right.custom_enchantments ||
        left.components != right.components ||
        left.attribute_modifiers.size() != right.attribute_modifiers.size() ||
        left.lore.size() != right.lore.size() ||
        left.custom_name.has_value() != right.custom_name.has_value())
      return false;
    for (std::size_t i = 0; i < left.attribute_modifiers.size(); ++i) {
      const ItemAttributeModifierValue& a = left.attribute_modifiers[i];
      const ItemAttributeModifierValue& b = right.attribute_modifiers[i];
      if (a.attribute != b.attribute || a.id != b.id || a.amount != b.amount ||
          a.operation != b.operation || a.slot != b.slot)
        return false;
    }
    if (left.custom_data.size() != right.custom_data.size())
      return false;
    for (const auto& [namespaced, fields] : left.custom_data) {
      const auto other_namespace = right.custom_data.find(namespaced);
      if (other_namespace == right.custom_data.end() ||
          other_namespace->second.size() != fields.size())
        return false;
      for (const auto& [key, value] : fields) {
        const auto other = other_namespace->second.find(key);
        if (other == other_namespace->second.end() || !same_nbt(value, other->second))
          return false;
      }
    }
    for (std::size_t i = 0; i < left.lore.size(); ++i) {
      if (!same_text(left.lore[i], right.lore[i]))
        return false;
    }
    if (left.custom_name && !same_text(*left.custom_name, *right.custom_name))
      return false;
    return true;
  }

  void drop_resource(std::uint32_t handle, ResourceKind kind) {
    const auto it = resources_.find(handle);
    if (it != resources_.end() && it->second == kind) {
      resources_.erase(it);
      if (kind == ResourceKind::Player)
        player_names_.erase(handle);
      else if (kind == ResourceKind::Text)
        text_values_.erase(handle);
      else if (kind == ResourceKind::Item)
        item_values_.erase(handle);
      else if (kind == ResourceKind::Command)
        command_values_.erase(handle);
      else if (kind == ResourceKind::CommandNode)
        command_node_values_.erase(handle);
      else if (kind == ResourceKind::World)
        world_names_.erase(handle);
    }
  }

  bool call_task(std::uint32_t handler_id, std::uint32_t server, std::string* error) {
    stack_.clear();
    frames_.clear();
    steps_ = 0;
    stack_.push_back(make_i32(handler_id));
    stack_.push_back(make_i32(server));
    call_index(task_callback_);
    if (task_callback_async_) {
      task_ = TaskResult{};
      try {
        execute();
      } catch (...) {
        report_->steps += steps_;
        throw;
      }
      report_->steps += steps_;
      if (!task_.called) {
        *error = "the async handle-task returned without calling task.return";
        return false;
      }
      if (!task_.ok) {
        *error = task_.error;
        return false;
      }
      return true;
    }
    try {
      execute();
    } catch (...) {
      report_->steps += steps_;
      throw;
    }
    report_->steps += steps_;
    const std::int32_t result = stack_.empty() ? 0 : as_i32(stack_.back());
    stack_.clear();
    if (task_callback_status_ && result != 0) {
      *error = "handle-task returned " + std::to_string(result);
      return false;
    }
    return true;
  }

  TaskResult task_;
  // Async-lowered host imports complete as one-shot subtasks. The task map is
  // keyed by the status handle the guest receives; waitable-set.wait delivers
  // its returned event and removes it from the set.
  std::map<std::uint32_t, bool> async_tasks_;
  std::map<std::uint32_t, std::vector<std::uint32_t>> waitable_sets_;
  std::uint32_t next_async_task_ = 1;
  std::uint32_t next_waitable_set_ = 1;

  void execute_async_host(const HostImport& import, const std::uint32_t* arguments,
                          std::size_t count) {
    switch (import.id) {
    case HostId::PlayerChat: {
      const std::string text = host_string(arguments[0], arguments[1]);
      record(&report_->messages, "", text);
      record_host_call("player_chat", static_cast<std::int64_t>(text.size()));
      host_.player_chat(text);
      return;
    }
    case HostId::PumpkinPlayerChat: {
      if (count < 3 || !valid_resource(arguments[0], ResourceKind::Player))
        trap("async player.chat used an invalid player handle");
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("async player.chat used a stale player handle");
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->messages, "", text);
      record_host_call("player_chat", static_cast<std::int64_t>(text.size()));
      host_.player_chat(text);
      return;
    }
    case HostId::PumpkinServerBroadcast: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("async server.broadcast used an invalid server handle");
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->messages, "", text);
      const std::int32_t sent = host_.broadcast(text);
      record_host_call("broadcast", sent);
      return;
    }
    case HostId::PumpkinWorldSetBlockAsync: {
      // The async ABI moves the five flattened `set-block` parameters behind
      // one in-pointer: x, y, z, block pointer, flags.
      const std::uint8_t* area = memory_at(arguments[0], 20);
      const std::int32_t x =
          static_cast<std::int32_t>(area[0] | (area[1] << 8) | (area[2] << 16) | (area[3] << 24));
      const std::int32_t y =
          static_cast<std::int32_t>(area[4] | (area[5] << 8) | (area[6] << 16) | (area[7] << 24));
      const std::int32_t z =
          static_cast<std::int32_t>(area[8] | (area[9] << 8) | (area[10] << 16) | (area[11] << 24));
      const std::uint32_t block_pointer =
          area[12] | (area[13] << 8) | (area[14] << 16) | (area[15] << 24);
      const std::uint8_t* block = memory_at(block_pointer, 56);
      const std::uint16_t state = static_cast<std::uint16_t>(block[38] | (block[39] << 8));
      const std::int32_t result = host_.world_set_block(x, y, z, state);
      record_host_call("world_set_block", result);
      return;
    }
    default:
      trap("an async lower is bound to a host call that has no async operation");
    }
  }

  void check_clock() {
    if (std::chrono::steady_clock::now() >= deadline_)
      budget("wall-clock budget exhausted (" + std::to_string(limits_.max_millis) + " ms)");
  }

  Value pop() {
    if (stack_.empty())
      trap("operand stack underflow");
    const Value value = stack_.back();
    stack_.pop_back();
    return value;
  }

  void push(const Value& value) {
    stack_.push_back(value);
  }
  void push_i32(std::uint32_t value) {
    stack_.push_back(make_i32(value));
  }

  std::uint8_t* memory_at(std::uint64_t address, std::uint64_t width) {
    const std::uint64_t end = address + width;
    if (end < address || end > memory_.size())
      trap("out of bounds memory access");
    return memory_.data() + address;
  }

  std::string host_string(std::uint32_t pointer, std::uint32_t length) {
    if (length == 0)
      return {};
    const std::uint8_t* bytes = memory_at(pointer, length);
    host_bytes_ += length;
    if (host_bytes_ > limits_.max_host_bytes)
      budget("host message budget exhausted (" + std::to_string(limits_.max_host_bytes) +
             " bytes)");
    return std::string(reinterpret_cast<const char*>(bytes), length);
  }

public:
  // A lifted string result: the core function returns a pointer to a
  // (pointer, length) pair in memory 0. `memory_at` bounds-checks the pair and
  // `host_string` bounds-checks and charges the bytes it names.
  std::string lift_string(std::uint32_t return_pointer) {
    const std::uint8_t* pair = memory_at(return_pointer, 8);
    const std::uint32_t pointer =
        static_cast<std::uint32_t>(pair[0]) | (static_cast<std::uint32_t>(pair[1]) << 8) |
        (static_cast<std::uint32_t>(pair[2]) << 16) | (static_cast<std::uint32_t>(pair[3]) << 24);
    const std::uint32_t length =
        static_cast<std::uint32_t>(pair[4]) | (static_cast<std::uint32_t>(pair[5]) << 8) |
        (static_cast<std::uint32_t>(pair[6]) << 16) | (static_cast<std::uint32_t>(pair[7]) << 24);
    return host_string(pointer, length);
  }

private:
  void record(std::vector<std::string>* lines, const std::string& prefix, const std::string& text) {
    if (lines->size() >= kMaxReportedLines)
      return;
    lines->push_back(prefix + text);
  }

  // `br`/`br_table`/`return` all need the same stack normalisation.
  void unwind_to(std::size_t height, std::size_t arity) {
    if (arity != 0) {
      if (stack_.size() <= height)
        trap("branch is missing its result value");
      stack_[height] = stack_.back();
    }
    stack_.resize(height + arity);
  }

  void do_return() {
    Frame& frame = frames_.back();
    const bool has_result = frame.type->has_result;
    const std::size_t base = frame.stack_base;
    unwind_to(base, has_result ? 1 : 0);
    frames_.pop_back();
  }

  const FuncType* function_type(std::uint32_t index) const {
    if (index < module_.imports.size()) {
      const std::uint32_t type_index = module_.imports[index].type_index;
      return type_index < module_.types.size() ? &module_.types[type_index] : nullptr;
    }
    const std::size_t defined = index - module_.imports.size();
    if (defined >= module_.funcs.size())
      return nullptr;
    const std::uint32_t type_index = module_.funcs[defined].type_index;
    return type_index < module_.types.size() ? &module_.types[type_index] : nullptr;
  }

  Table& table_at(std::uint32_t index) {
    if (index >= tables_.size())
      trap("table " + std::to_string(index) + " does not exist");
    return tables_[index];
  }

  void call_index(std::uint32_t index) {
    if (index < module_.imports.size()) {
      call_host(index);
      return;
    }
    const std::size_t defined = index - module_.imports.size();
    if (defined >= module_.funcs.size())
      trap("call to a function that does not exist");
    const FuncBody& function = module_.funcs[defined];
    if (function.type_index >= module_.types.size())
      trap("function has no type");
    const FuncType& type = module_.types[function.type_index];
    if (stack_.size() < type.params.size())
      trap("call is missing its arguments");
    const std::size_t base = stack_.size() - type.params.size();

    Frame frame;
    frame.function = &function;
    frame.type = &type;
    frame.body = module_.bytes.data() + function.body_offset;
    frame.size = function.body_size;
    frame.stack_base = base;
    frame.locals.resize(type.params.size() + function.locals.size());
    for (std::size_t i = 0; i < type.params.size(); ++i)
      frame.locals[i] = stack_[base + i];
    for (std::size_t i = 0; i < function.locals.size(); ++i)
      frame.locals[type.params.size() + i] = zero_of(function.locals[i]);
    stack_.resize(base);

    frames_.push_back(std::move(frame));
    if (frames_.size() > limits_.max_call_depth) {
      frames_.pop_back();
      budget("call depth budget exhausted (" + std::to_string(limits_.max_call_depth) + " frames)");
    }
    if (frames_.size() > report_->peak_depth)
      report_->peak_depth = frames_.size();
  }

  void record_host_call(std::string_view name, std::int64_t value) {
    report_->host_calls.push_back(std::string(name) + "=" + std::to_string(value));
  }

  // A coordinate read is only observable if the arguments survive the canonical
  // ABI, so record them alongside the returned state id.
  void record_host_position(std::string_view name, std::int32_t x, std::int32_t y, std::int32_t z) {
    report_->host_calls.push_back(std::string(name) + "=" + std::to_string(x) + "," +
                                  std::to_string(y) + "," + std::to_string(z));
  }

  // Calls the module's `cabi_realloc` as a nested guest call. The outer
  // instruction loop keeps its frame; `execute(stop_depth)` runs only the
  // realloc frame and returns when it has been popped.
  std::uint32_t call_realloc(const HostImport& import, std::uint32_t old_pointer,
                             std::uint32_t old_size, std::uint32_t align, std::uint32_t new_size) {
    if (!import.has_realloc)
      trap("a host result needs a realloc canonical option");
    if (import.realloc < module_.imports.size() || import.realloc >= module_.function_count())
      trap("cabi_realloc does not name a function the module defines");
    const std::size_t base = frames_.size();
    stack_.push_back(make_i32(old_pointer));
    stack_.push_back(make_i32(old_size));
    stack_.push_back(make_i32(align));
    stack_.push_back(make_i32(new_size));
    call_index(import.realloc);
    execute(base);
    if (stack_.empty())
      trap("cabi_realloc returned no value");
    const std::uint32_t result = as_u32(stack_.back());
    stack_.pop_back();
    return result;
  }

  void write_string_pair(const HostImport& import, std::uint32_t target, std::string_view value) {
    if (!import.has_realloc)
      trap("a host result with a string payload needs a realloc");
    std::uint32_t string = 0;
    if (!value.empty()) {
      string = call_realloc(import, 0, 0, 1, static_cast<std::uint32_t>(value.size()));
      if (string == 0)
        trap("cabi_realloc returned a null string pointer");
      std::uint8_t* bytes = memory_at(string, value.size());
      std::memcpy(bytes, value.data(), value.size());
      host_bytes_ += value.size();
      if (host_bytes_ > limits_.max_host_bytes)
        budget("host message budget exhausted (" + std::to_string(limits_.max_host_bytes) +
               " bytes)");
    }
    std::uint8_t* pair = memory_at(target, 8);
    const std::uint32_t length = static_cast<std::uint32_t>(value.size());
    for (unsigned int i = 0; i < 4; ++i) {
      pair[i] = static_cast<std::uint8_t>((string >> (8 * i)) & 0xff);
      pair[4 + i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xff);
    }
  }

  std::uint32_t allocate_string(const HostImport& import, std::string_view value) {
    const std::uint32_t area = call_realloc(import, 0, 0, 4, 8);
    if (area == 0)
      trap("cabi_realloc returned a null return area");
    write_string_pair(import, area, value);
    return area;
  }

  std::uint32_t allocate_string_list(const HostImport& import,
                                     const std::vector<std::string>& values) {
    const std::uint32_t area = call_realloc(import, 0, 0, 4, 8);
    if (area == 0)
      trap("cabi_realloc returned a null list return area");
    if (values.size() > 0x1fffffffu)
      trap("a string list is too large for the hosted ABI");
    std::uint32_t array = 0;
    if (!values.empty()) {
      array = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(values.size() * 8));
      if (array == 0)
        trap("cabi_realloc returned a null list pointer");
    }
    for (std::size_t index = 0; index < values.size(); ++index)
      write_string_pair(import, array + static_cast<std::uint32_t>(index * 8), values[index]);
    std::uint8_t* list = memory_at(area, 8);
    const std::uint32_t count = static_cast<std::uint32_t>(values.size());
    for (unsigned int i = 0; i < 4; ++i) {
      list[i] = static_cast<std::uint8_t>((array >> (8 * i)) & 0xff);
      list[4 + i] = static_cast<std::uint8_t>((count >> (8 * i)) & 0xff);
    }
    return area;
  }

  void write_option_u32(std::uint32_t target, const std::optional<std::uint32_t>& value) {
    std::uint8_t* option = memory_at(target, 8);
    std::memset(option, 0, 8);
    if (value) {
      option[0] = 1;
      write_u32(target + 4, *value);
    }
  }

  void write_option_handle(std::uint32_t target, std::optional<std::uint32_t> value) {
    std::uint8_t* option = memory_at(target, 8);
    std::memset(option, 0, 8);
    if (value) {
      option[0] = 1;
      write_u32(target + 4, *value);
    }
  }

  void write_handle_list(const HostImport& import, std::uint32_t target,
                         const std::vector<std::uint32_t>& handles) {
    std::uint32_t array = 0;
    if (!handles.empty()) {
      array = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(handles.size() * 4));
      if (array == 0)
        trap("cabi_realloc returned a null resource list");
      for (std::size_t i = 0; i < handles.size(); ++i)
        write_u32(array + static_cast<std::uint32_t>(i * 4), handles[i]);
    }
    write_u32(target, array);
    write_u32(target + 4, static_cast<std::uint32_t>(handles.size()));
  }

  void write_enchantment_list(const HostImport& import, std::uint32_t target,
                              const std::map<std::uint32_t, std::uint32_t>& values) {
    std::uint32_t array = 0;
    if (!values.empty()) {
      array = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(values.size() * 8));
      if (array == 0)
        trap("cabi_realloc returned a null enchantment list");
      std::uint32_t index = 0;
      for (const auto& [enchantment, level] : values) {
        write_u32(array + index * 8, enchantment);
        write_u32(array + index * 8 + 4, level);
        ++index;
      }
    }
    write_u32(target, array);
    write_u32(target + 4, static_cast<std::uint32_t>(values.size()));
  }

  void write_custom_enchantment_list(const HostImport& import, std::uint32_t target,
                                     const std::map<std::string, std::uint32_t>& values) {
    std::uint32_t array = 0;
    if (!values.empty()) {
      array = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(values.size() * 12));
      if (array == 0)
        trap("cabi_realloc returned a null custom enchantment list");
      std::uint32_t index = 0;
      for (const auto& [id, level] : values) {
        write_string_pair(import, array + index * 12, id);
        write_u32(array + index * 12 + 8, level);
        ++index;
      }
    }
    write_u32(target, array);
    write_u32(target + 4, static_cast<std::uint32_t>(values.size()));
  }

  void write_attribute_modifier_list(const HostImport& import, std::uint32_t target,
                                     const std::vector<ItemAttributeModifierValue>& values) {
    std::uint32_t array = 0;
    if (!values.empty()) {
      array = call_realloc(import, 0, 0, 8, static_cast<std::uint32_t>(values.size() * 40));
      if (array == 0)
        trap("cabi_realloc returned a null attribute-modifier list");
      for (std::size_t i = 0; i < values.size(); ++i) {
        const std::uint32_t base = array + static_cast<std::uint32_t>(i * 40);
        std::memset(memory_at(base, 40), 0, 40);
        const ItemAttributeModifierValue& value = values[i];
        memory_at(base, 1)[0] = static_cast<std::uint8_t>(value.attribute);
        write_string_pair(import, base + 8, value.id);
        write_f64(base + 16, value.amount);
        memory_at(base + 24, 1)[0] = static_cast<std::uint8_t>(value.operation);
        memory_at(base + 32, 1)[0] = static_cast<std::uint8_t>(value.slot);
      }
      charge_host_bytes(values.size() * 40);
    }
    write_u32(target, array);
    write_u32(target + 4, static_cast<std::uint32_t>(values.size()));
  }

  void write_component_list(const HostImport& import, std::uint32_t target,
                            const std::map<std::uint32_t, std::vector<std::uint8_t>>& values) {
    std::uint32_t array = 0;
    if (!values.empty()) {
      array = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(values.size() * 12));
      if (array == 0)
        trap("cabi_realloc returned a null data-component list");
      std::uint32_t index = 0;
      for (const auto& [component, bytes] : values) {
        const std::uint32_t base = array + index * 12;
        std::memset(memory_at(base, 12), 0, 12);
        write_u32(base, component);
        std::uint32_t pointer = 0;
        if (!bytes.empty()) {
          pointer = call_realloc(import, 0, 0, 1, static_cast<std::uint32_t>(bytes.size()));
          if (pointer == 0)
            trap("cabi_realloc returned a null data-component value");
          std::memcpy(memory_at(pointer, bytes.size()), bytes.data(), bytes.size());
          charge_host_bytes(bytes.size());
        }
        write_u32(base + 4, pointer);
        write_u32(base + 8, static_cast<std::uint32_t>(bytes.size()));
        ++index;
      }
      charge_host_bytes(values.size() * 12);
    }
    write_u32(target, array);
    write_u32(target + 4, static_cast<std::uint32_t>(values.size()));
  }

  void write_option_nbt(const HostImport& import, std::uint32_t target, const nbt::Value* value) {
    std::uint8_t* option = memory_at(target, 16);
    std::memset(option, 0, 16);
    if (value == nullptr)
      return;
    const NbtTreeOutput out = allocate_nbt_tree(import, *value);
    // `allocate_nbt_tree` may grow memory, which can move the return area.
    memory_at(target, 1)[0] = 1;
    write_u32(target + 4, out.root);
    write_u32(target + 8, out.tags);
    write_u32(target + 12, out.count);
  }

  std::uint16_t read_u16(std::uint32_t target) {
    const std::uint8_t* bytes = memory_at(target, 2);
    return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
  }

  std::uint32_t read_u32(std::uint32_t target) {
    const std::uint8_t* bytes = memory_at(target, 4);
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
  }

  std::string read_string_pair(std::uint32_t target) {
    const std::uint32_t pointer = read_u32(target);
    const std::uint32_t length = read_u32(target + 4);
    if (length == 0)
      return {};
    const std::uint8_t* bytes = memory_at(pointer, length);
    return std::string(reinterpret_cast<const char*>(bytes), length);
  }

  std::uint64_t read_u64(std::uint32_t target) {
    const std::uint8_t* bytes = memory_at(target, 8);
    std::uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
      value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
  }

  void charge_host_bytes(std::size_t bytes) {
    host_bytes_ += bytes;
    if (host_bytes_ > limits_.max_host_bytes)
      budget("host message budget exhausted (" + std::to_string(limits_.max_host_bytes) +
             " bytes)");
  }

  static constexpr std::uint32_t kMaxNbtTags = 100000;
  static constexpr std::uint32_t kMaxNbtItems = 1000000;
  static constexpr unsigned int kMaxNbtDepth = 64;

  nbt::Value decode_nbt(std::uint32_t index, std::uint32_t tags, std::uint32_t count,
                        std::vector<std::uint8_t>& visiting, unsigned int depth) {
    if (depth > kMaxNbtDepth)
      trap("an nbt-tree is nested deeper than the host allows");
    if (index >= count)
      trap("an nbt-tree names a tag outside its tags list");
    if (visiting[index] != 0)
      trap("an nbt-tree contains a cycle");
    visiting[index] = 1;
    const std::uint32_t base = tags + index * 16;
    const std::uint8_t kind = memory_at(base, 1)[0];
    nbt::Value value;
    switch (kind) {
    case 0:
      value = nbt::make_byte(static_cast<std::int8_t>(memory_at(base + 8, 1)[0]));
      break;
    case 1:
      value = nbt::make_short(static_cast<std::int16_t>(read_u16(base + 8)));
      break;
    case 2:
      value = nbt::make_int(static_cast<std::int32_t>(read_u32(base + 8)));
      break;
    case 3: {
      const std::uint64_t bits = read_u64(base + 8);
      std::int64_t number = 0;
      std::memcpy(&number, &bits, sizeof(number));
      value = nbt::make_long(number);
      break;
    }
    case 4: {
      const std::uint32_t bits = read_u32(base + 8);
      float number = 0;
      std::memcpy(&number, &bits, sizeof(number));
      value = nbt::make_float(number);
      break;
    }
    case 5: {
      const std::uint64_t bits = read_u64(base + 8);
      double number = 0;
      std::memcpy(&number, &bits, sizeof(number));
      value = nbt::make_double(number);
      break;
    }
    case 6: {
      const std::uint32_t pointer = read_u32(base + 8);
      const std::uint32_t length = read_u32(base + 12);
      if (length > kMaxNbtItems)
        trap("an nbt byte-array is larger than the host allows");
      std::vector<std::int8_t> bytes(length);
      if (length != 0) {
        const std::uint8_t* source = memory_at(pointer, length);
        std::memcpy(bytes.data(), source, length);
        charge_host_bytes(length);
      }
      value = nbt::make_byte_array(std::move(bytes));
      break;
    }
    case 7: {
      value = nbt::make_string(host_string(read_u32(base + 8), read_u32(base + 12)));
      break;
    }
    case 8: {
      const std::uint32_t pointer = read_u32(base + 8);
      const std::uint32_t length = read_u32(base + 12);
      if (length > kMaxNbtItems)
        trap("an nbt list is larger than the host allows");
      std::vector<nbt::Value> items;
      items.reserve(length);
      nbt::Tag element_type = nbt::Tag::End;
      for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t child = read_u32(pointer + i * 4);
        nbt::Value item = decode_nbt(child, tags, count, visiting, depth + 1);
        if (i == 0)
          element_type = item.type;
        else if (item.type != element_type)
          trap("an nbt list contains mixed tag types");
        items.push_back(std::move(item));
      }
      value = nbt::make_list(element_type, std::move(items));
      break;
    }
    case 9: {
      const std::uint32_t pointer = read_u32(base + 8);
      const std::uint32_t length = read_u32(base + 12);
      if (length > kMaxNbtItems)
        trap("an nbt compound is larger than the host allows");
      std::vector<std::pair<std::string, nbt::Value>> fields;
      fields.reserve(length);
      for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint32_t entry = pointer + i * 12;
        std::string key = host_string(read_u32(entry), read_u32(entry + 4));
        fields.emplace_back(std::move(key),
                            decode_nbt(read_u32(entry + 8), tags, count, visiting, depth + 1));
      }
      value = nbt::make_compound(std::move(fields));
      break;
    }
    case 10: {
      const std::uint32_t pointer = read_u32(base + 8);
      const std::uint32_t length = read_u32(base + 12);
      if (length > kMaxNbtItems)
        trap("an nbt int-array is larger than the host allows");
      std::vector<std::int32_t> ints(length);
      for (std::uint32_t i = 0; i < length; ++i)
        ints[i] = static_cast<std::int32_t>(read_u32(pointer + i * 4));
      if (length != 0)
        charge_host_bytes(static_cast<std::size_t>(length) * 4);
      value = nbt::make_int_array(std::move(ints));
      break;
    }
    case 11: {
      const std::uint32_t pointer = read_u32(base + 8);
      const std::uint32_t length = read_u32(base + 12);
      if (length > kMaxNbtItems)
        trap("an nbt long-array is larger than the host allows");
      std::vector<std::int64_t> longs(length);
      for (std::uint32_t i = 0; i < length; ++i) {
        const std::uint64_t bits = read_u64(pointer + i * 8);
        std::memcpy(&longs[i], &bits, sizeof(longs[i]));
      }
      if (length != 0)
        charge_host_bytes(static_cast<std::size_t>(length) * 8);
      value = nbt::make_long_array(std::move(longs));
      break;
    }
    default:
      trap("an nbt-tree tag has an unknown variant");
    }
    visiting[index] = 0;
    return value;
  }

  nbt::Value read_nbt_tree(std::uint32_t root, std::uint32_t tags, std::uint32_t count) {
    if (count == 0 || count > kMaxNbtTags)
      trap("an nbt-tree has an invalid tag count");
    if (root >= count)
      trap("an nbt-tree root is outside its tags list");
    std::vector<std::uint8_t> visiting(count, 0);
    return decode_nbt(root, tags, count, visiting, 0);
  }

  struct OutNbtTag {
    std::uint8_t kind = 0;
    std::int64_t integer = 0;
    double floating = 0;
    std::string text;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint32_t> refs;
    std::vector<std::pair<std::string, std::uint32_t>> fields;
    std::vector<std::int32_t> ints;
    std::vector<std::int64_t> longs;
  };

  void append_nbt(const nbt::Value& value, std::vector<OutNbtTag>* out, unsigned int depth) {
    if (depth > kMaxNbtDepth)
      trap("an nbt-tree is nested deeper than the host allows");
    if (value.type == nbt::Tag::End)
      trap("an nbt-tree cannot contain an end tag");
    OutNbtTag tag;
    switch (value.type) {
    case nbt::Tag::Byte:
      tag.kind = 0;
      tag.integer = static_cast<std::int8_t>(value.i);
      break;
    case nbt::Tag::Short:
      tag.kind = 1;
      tag.integer = static_cast<std::int16_t>(value.i);
      break;
    case nbt::Tag::Int:
      tag.kind = 2;
      tag.integer = static_cast<std::int32_t>(value.i);
      break;
    case nbt::Tag::Long:
      tag.kind = 3;
      tag.integer = value.i;
      break;
    case nbt::Tag::Float:
      tag.kind = 4;
      tag.floating = static_cast<float>(value.d);
      break;
    case nbt::Tag::Double:
      tag.kind = 5;
      tag.floating = value.d;
      break;
    case nbt::Tag::ByteArray:
      tag.kind = 6;
      tag.bytes.assign(value.bytes.begin(), value.bytes.end());
      break;
    case nbt::Tag::String:
      tag.kind = 7;
      tag.text = value.s;
      break;
    case nbt::Tag::List:
      tag.kind = 8;
      break;
    case nbt::Tag::Compound:
      tag.kind = 9;
      break;
    case nbt::Tag::IntArray:
      tag.kind = 10;
      tag.ints = value.ints;
      break;
    case nbt::Tag::LongArray:
      tag.kind = 11;
      tag.longs = value.longs;
      break;
    case nbt::Tag::End:
      break;
    }
    const std::uint32_t index = static_cast<std::uint32_t>(out->size());
    out->push_back(std::move(tag));
    if (value.type == nbt::Tag::List) {
      for (const nbt::Value& item : value.items) {
        const std::uint32_t child = static_cast<std::uint32_t>(out->size());
        (*out)[index].refs.push_back(child);
        append_nbt(item, out, depth + 1);
      }
    } else if (value.type == nbt::Tag::Compound) {
      for (const auto& [key, child_value] : value.fields) {
        const std::uint32_t child = static_cast<std::uint32_t>(out->size());
        (*out)[index].fields.emplace_back(key, child);
        append_nbt(child_value, out, depth + 1);
      }
    }
    if (out->size() > kMaxNbtTags)
      trap("an nbt-tree has more tags than the host allows");
  }

  struct NbtTreeOutput {
    std::uint32_t root = 0;
    std::uint32_t tags = 0;
    std::uint32_t count = 0;
  };

  NbtTreeOutput allocate_nbt_tree(const HostImport& import, const nbt::Value& value) {
    std::vector<OutNbtTag> tags;
    tags.reserve(8);
    append_nbt(value, &tags, 0);
    NbtTreeOutput output;
    output.root = 0;
    output.count = static_cast<std::uint32_t>(tags.size());
    output.tags = call_realloc(import, 0, 0, 8, output.count * 16);
    if (output.tags == 0)
      trap("cabi_realloc returned a null nbt tags pointer");
    std::memset(memory_at(output.tags, output.count * 16), 0, output.count * 16);
    charge_host_bytes(static_cast<std::size_t>(output.count) * 16);
    for (std::uint32_t i = 0; i < output.count; ++i) {
      const std::uint32_t base = output.tags + i * 16;
      const OutNbtTag& tag = tags[i];
      memory_at(base, 1)[0] = tag.kind;
      switch (tag.kind) {
      case 0:
        memory_at(base + 8, 1)[0] = static_cast<std::uint8_t>(tag.integer);
        break;
      case 1:
        write_u16(base + 8, static_cast<std::uint16_t>(tag.integer));
        break;
      case 2:
        write_u32(base + 8, static_cast<std::uint32_t>(tag.integer));
        break;
      case 3:
        write_u64(base + 8, static_cast<std::uint64_t>(tag.integer));
        break;
      case 4:
        write_f32(base + 8, static_cast<float>(tag.floating));
        break;
      case 5:
        write_f64(base + 8, tag.floating);
        break;
      case 6: {
        std::uint32_t pointer = 0;
        if (!tag.bytes.empty()) {
          pointer = call_realloc(import, 0, 0, 1, static_cast<std::uint32_t>(tag.bytes.size()));
          if (pointer == 0)
            trap("cabi_realloc returned a null nbt byte-array pointer");
          std::memcpy(memory_at(pointer, tag.bytes.size()), tag.bytes.data(), tag.bytes.size());
          charge_host_bytes(tag.bytes.size());
        }
        write_u32(base + 8, pointer);
        write_u32(base + 12, static_cast<std::uint32_t>(tag.bytes.size()));
        break;
      }
      case 7:
        write_string_pair(import, base + 8, tag.text);
        break;
      case 8: {
        std::uint32_t pointer = 0;
        if (!tag.refs.empty()) {
          pointer = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(tag.refs.size() * 4));
          if (pointer == 0)
            trap("cabi_realloc returned a null nbt list pointer");
          for (std::size_t j = 0; j < tag.refs.size(); ++j)
            write_u32(pointer + static_cast<std::uint32_t>(j * 4), tag.refs[j]);
          charge_host_bytes(tag.refs.size() * 4);
        }
        write_u32(base + 8, pointer);
        write_u32(base + 12, static_cast<std::uint32_t>(tag.refs.size()));
        break;
      }
      case 9: {
        std::uint32_t pointer = 0;
        if (!tag.fields.empty()) {
          pointer =
              call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(tag.fields.size() * 12));
          if (pointer == 0)
            trap("cabi_realloc returned a null nbt compound pointer");
          for (std::size_t j = 0; j < tag.fields.size(); ++j) {
            const std::uint32_t entry = pointer + static_cast<std::uint32_t>(j * 12);
            write_string_pair(import, entry, tag.fields[j].first);
            write_u32(entry + 8, tag.fields[j].second);
          }
          charge_host_bytes(tag.fields.size() * 12);
        }
        write_u32(base + 8, pointer);
        write_u32(base + 12, static_cast<std::uint32_t>(tag.fields.size()));
        break;
      }
      case 10: {
        std::uint32_t pointer = 0;
        if (!tag.ints.empty()) {
          pointer = call_realloc(import, 0, 0, 4, static_cast<std::uint32_t>(tag.ints.size() * 4));
          if (pointer == 0)
            trap("cabi_realloc returned a null nbt int-array pointer");
          for (std::size_t j = 0; j < tag.ints.size(); ++j)
            write_u32(pointer + static_cast<std::uint32_t>(j * 4),
                      static_cast<std::uint32_t>(tag.ints[j]));
          charge_host_bytes(tag.ints.size() * 4);
        }
        write_u32(base + 8, pointer);
        write_u32(base + 12, static_cast<std::uint32_t>(tag.ints.size()));
        break;
      }
      case 11: {
        std::uint32_t pointer = 0;
        if (!tag.longs.empty()) {
          pointer = call_realloc(import, 0, 0, 8, static_cast<std::uint32_t>(tag.longs.size() * 8));
          if (pointer == 0)
            trap("cabi_realloc returned a null nbt long-array pointer");
          for (std::size_t j = 0; j < tag.longs.size(); ++j)
            write_u64(pointer + static_cast<std::uint32_t>(j * 8),
                      static_cast<std::uint64_t>(tag.longs[j]));
          charge_host_bytes(tag.longs.size() * 8);
        }
        write_u32(base + 8, pointer);
        write_u32(base + 12, static_cast<std::uint32_t>(tag.longs.size()));
        break;
      }
      default:
        trap("an nbt-tree tag has an unknown variant");
      }
    }
    return output;
  }

  void write_f64(std::uint32_t target, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_u64(target, bits);
  }

  void write_option_bool(std::uint32_t target, const std::optional<bool>& value) {
    std::uint8_t* option = memory_at(target, 2);
    option[0] = value ? 1 : 0;
    option[1] = value && *value ? 1 : 0;
  }

  // Pumpkin v0.2's `marketplace-metadata` record is 88 bytes, aligned to 8;
  // `license-key` is an inline option<string> at byte 64 and every string is
  // allocated through the module's cabi_realloc.
  void write_marketplace_metadata(const HostImport& import, std::uint32_t base,
                                  const MarketplaceMetadata& metadata) {
    write_string_pair(import, base, metadata.marketplace_url);
    write_u64(base + 8, static_cast<std::uint64_t>(metadata.plugin_id));
    write_string_pair(import, base + 16, metadata.plugin_name);
    write_string_pair(import, base + 24, metadata.version);
    write_u64(base + 32, static_cast<std::uint64_t>(metadata.dev_id));
    write_string_pair(import, base + 40, metadata.dev_name);
    memory_at(base + 48, 1)[0] = metadata.is_paid ? 1 : 0;
    write_u64(base + 56, static_cast<std::uint64_t>(metadata.user_id));
    std::memset(memory_at(base + 64, 12), 0, 12);
    if (metadata.license_key) {
      memory_at(base + 64, 1)[0] = 1;
      write_string_pair(import, base + 68, *metadata.license_key);
    }
    write_string_pair(import, base + 76, metadata.issued_at);
  }

  std::uint32_t allocate_string_record(const HostImport& import, std::string_view key,
                                       std::string_view value) {
    const std::uint32_t area = call_realloc(import, 0, 0, 4, 16);
    if (area == 0)
      trap("cabi_realloc returned a null record return area");
    write_string_pair(import, area, key);
    write_string_pair(import, area + 8, value);
    return area;
  }

  void write_u16(std::uint32_t target, std::uint16_t value) {
    std::uint8_t* out = memory_at(target, 2);
    out[0] = static_cast<std::uint8_t>(value & 0xff);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
  }

  void write_u32(std::uint32_t target, std::uint32_t value) {
    std::uint8_t* out = memory_at(target, 4);
    for (unsigned int i = 0; i < 4; ++i)
      out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
  }

  void write_u64(std::uint32_t target, std::uint64_t value) {
    std::uint8_t* out = memory_at(target, 8);
    for (unsigned int i = 0; i < 8; ++i)
      out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
  }

  void write_f32(std::uint32_t target, float value) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(target, bits);
  }

  // The canonical layout of Pumpkin v0.2's `block` record: 56 bytes, aligned
  // to 4. The name and the state-id array are allocations; the optional
  // `flammable` record is inline at byte 51 (discriminant at 51, spread at 52,
  // burn at 53, with the last two bytes padding the record to 56).
  std::uint32_t allocate_block(const HostImport& import, const BlockInfo& block) {
    const std::uint32_t area = call_realloc(import, 0, 0, 4, 56);
    if (area == 0)
      trap("cabi_realloc returned a null block return area");
    std::memset(memory_at(area, 56), 0, 56);

    write_u16(area + 0, block.id);
    write_string_pair(import, area + 4, block.name);
    write_f32(area + 12, block.hardness);
    write_f32(area + 16, block.blast_resistance);
    memory_at(area + 20, 1)[0] = block.map_color;
    write_f32(area + 24, block.slipperiness);
    write_f32(area + 28, block.velocity_multiplier);
    write_f32(area + 32, block.jump_velocity_multiplier);
    write_u16(area + 36, block.item_id);
    write_u16(area + 38, block.default_state_id);

    std::uint32_t array = 0;
    if (!block.state_ids.empty()) {
      const std::uint32_t bytes = static_cast<std::uint32_t>(block.state_ids.size() * 2);
      array = call_realloc(import, 0, 0, 2, bytes);
      if (array == 0)
        trap("cabi_realloc returned a null block state-id array");
      for (std::size_t i = 0; i < block.state_ids.size(); ++i)
        write_u16(array + static_cast<std::uint32_t>(i * 2), block.state_ids[i]);
      host_bytes_ += bytes;
      if (host_bytes_ > limits_.max_host_bytes)
        budget("host message budget exhausted (" + std::to_string(limits_.max_host_bytes) +
               " bytes)");
    }
    write_u32(area + 40, array);
    write_u32(area + 44, static_cast<std::uint32_t>(block.state_ids.size()));
    memory_at(area + 48, 1)[0] = block.is_solid ? 1 : 0;
    memory_at(area + 49, 1)[0] = block.is_air ? 1 : 0;
    memory_at(area + 50, 1)[0] = block.is_flammable ? 1 : 0;
    if (block.has_flammable) {
      memory_at(area + 51, 1)[0] = 1;
      memory_at(area + 52, 1)[0] = block.spread_chance;
      memory_at(area + 53, 1)[0] = block.burn_chance;
    }
    return area;
  }

  void call_host(std::uint32_t index) {
    const HostImport& import = module_.imports[index];
    const FuncType& type = module_.types[import.type_index];
    if (import.id == HostId::Stub) {
      // A compiled component can declare the whole Pumpkin surface without
      // calling most of it. Unbacked members are still not approximated: the
      // call traps with the exact import name if the guest reaches it.
      trap("host import " + import.module + "/" + import.member +
           " has no Pumpkin semantics in this build");
    }
    std::uint32_t arguments[12] = {};
    std::uint64_t argument_bits[12] = {};
    const std::size_t count = type.params.size();
    if (count > 12)
      trap("a host import has more parameters than the ABI allows");
    for (std::size_t i = count; i > 0; --i) {
      const Value value = pop();
      argument_bits[i - 1] = value.bits;
      arguments[i - 1] = as_u32(value);
    }

    if (import.async_lower) {
      const std::uint32_t task = next_async_task_++;
      execute_async_host(import, arguments, count);
      async_tasks_[task] = true;
      push_i32((task << 4) | 1);
      return;
    }

    switch (import.id) {
    case HostId::Log: {
      const std::string text = host_string(arguments[0], arguments[1]);
      record(&report_->log, "log: ", text);
      host_.log(text);
      return;
    }
    case HostId::LogLevel: {
      const std::int32_t level = static_cast<std::int32_t>(arguments[0]);
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->log, "log: ", text);
      record_host_call("log_level", level);
      host_.log_level(level, text);
      return;
    }
    case HostId::LogTracing: {
      const std::string event = host_string(arguments[0], arguments[1]);
      std::string hex;
      hex.reserve(event.size() * 3);
      char byte[4];
      for (unsigned char value : event) {
        std::snprintf(byte, sizeof(byte), "%02x", value);
        if (!hex.empty())
          hex.push_back(' ');
        hex += byte;
      }
      record(&report_->log, "trace: ", hex);
      record_host_call("log_tracing", static_cast<std::int64_t>(event.size()));
      host_.log_tracing(event);
      return;
    }
    case HostId::PlayerChat: {
      const std::string text = host_string(arguments[0], arguments[1]);
      record(&report_->messages, "", text);
      record_host_call("player_chat", static_cast<std::int64_t>(text.size()));
      host_.player_chat(text);
      return;
    }
    case HostId::PumpkinPlayerGetAbsorption: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-absorption used an invalid player handle " +
             std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-absorption used a stale player handle " +
             std::to_string(arguments[0]));
      float absorption = 0.0f;
      if (!host_.player_absorption(player->second, &absorption))
        trap("player.get-absorption has no live absorption for " + player->second);
      push(make_f32(absorption));
      record_host_call("player_get_absorption", static_cast<std::int64_t>(absorption));
      return;
    }
    case HostId::PumpkinPlayerGetExperienceLevel:
    case HostId::PumpkinPlayerGetExperienceProgress:
    case HostId::PumpkinPlayerGetExperiencePoints: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player experience used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player experience used a stale player handle " + std::to_string(arguments[0]));
      PlayerExperience experience;
      if (!host_.player_experience(player->second, &experience))
        trap("player experience has no live values for " + player->second);
      if (import.id == HostId::PumpkinPlayerGetExperienceLevel) {
        push_i32(static_cast<std::uint32_t>(experience.level));
        record_host_call("player_get_experience_level", experience.level);
      } else if (import.id == HostId::PumpkinPlayerGetExperienceProgress) {
        push(make_f32(experience.progress));
        record_host_call("player_get_experience_progress",
                         static_cast<std::int64_t>(experience.progress));
      } else {
        push_i32(static_cast<std::uint32_t>(experience.points));
        record_host_call("player_get_experience_points", experience.points);
      }
      return;
    }
    case HostId::PumpkinPlayerGetId: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-id used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-id used a stale player handle " + std::to_string(arguments[0]));
      std::uint64_t high = 0;
      std::uint64_t low = 0;
      if (!host_.player_uuid(player->second, &high, &low))
        trap("player.get-id has no live UUID for " + player->second);
      memory_at(arguments[1], 16);
      write_u64(arguments[1], high);
      write_u64(arguments[1] + 8, low);
      record_host_call("player_get_id", 1);
      return;
    }
    case HostId::PumpkinPlayerGetName: {
      // The handle was issued by `server.get-player-by-name`, so its canonical
      // name is the host's own registry spelling, not a guest-supplied string.
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-name used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-name used a stale player handle " + std::to_string(arguments[0]));
      write_string_pair(import, arguments[1], player->second);
      record_host_call("player_get_name", static_cast<std::int64_t>(player->second.size()));
      return;
    }
    case HostId::PumpkinPlayerGetPosition: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-position used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-position used a stale player handle " + std::to_string(arguments[0]));
      PlayerPose pose;
      if (!host_.player_pose(player->second, &pose))
        trap("player.get-position has no live pose for " + player->second);
      memory_at(arguments[1], 24);
      write_f64(arguments[1], pose.x);
      write_f64(arguments[1] + 8, pose.y);
      write_f64(arguments[1] + 16, pose.z);
      record_host_call("player_get_position_x", static_cast<std::int64_t>(pose.x));
      record_host_call("player_get_position_y", static_cast<std::int64_t>(pose.y));
      record_host_call("player_get_position_z", static_cast<std::int64_t>(pose.z));
      return;
    }
    case HostId::PumpkinPlayerGetYaw: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-yaw used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-yaw used a stale player handle " + std::to_string(arguments[0]));
      PlayerPose pose;
      if (!host_.player_pose(player->second, &pose))
        trap("player.get-yaw has no live pose for " + player->second);
      push(make_f32(pose.yaw));
      record_host_call("player_get_yaw", static_cast<std::int64_t>(pose.yaw));
      return;
    }
    case HostId::PumpkinPlayerGetPitch: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-pitch used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-pitch used a stale player handle " + std::to_string(arguments[0]));
      PlayerPose pose;
      if (!host_.player_pose(player->second, &pose))
        trap("player.get-pitch has no live pose for " + player->second);
      push(make_f32(pose.pitch));
      record_host_call("player_get_pitch", static_cast<std::int64_t>(pose.pitch));
      return;
    }
    case HostId::PumpkinPlayerGetGamemode: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-gamemode used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-gamemode used a stale player handle " + std::to_string(arguments[0]));
      std::int32_t gamemode = 0;
      if (!host_.player_gamemode(player->second, &gamemode))
        trap("player.get-gamemode has no live gamemode for " + player->second);
      push_i32(static_cast<std::uint32_t>(gamemode));
      record_host_call("player_get_gamemode", gamemode);
      return;
    }
    case HostId::PumpkinPlayerGetHealth: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-health used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-health used a stale player handle " + std::to_string(arguments[0]));
      float health = 0.0f;
      if (!host_.player_health(player->second, &health))
        trap("player.get-health has no live health for " + player->second);
      push(make_f32(health));
      record_host_call("player_get_health", static_cast<std::int64_t>(health));
      return;
    }
    case HostId::PumpkinPlayerGetMaxHealth:
    case HostId::PumpkinPlayerGetFoodLevel:
    case HostId::PumpkinPlayerGetSaturation:
    case HostId::PumpkinPlayerGetExhaustion: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player vitals used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player vitals used a stale player handle " + std::to_string(arguments[0]));
      PlayerVitals vitals;
      if (!host_.player_vitals(player->second, &vitals))
        trap("player vitals have no live values for " + player->second);
      if (import.id == HostId::PumpkinPlayerGetMaxHealth) {
        push(make_f32(vitals.max_health));
        record_host_call("player_get_max_health", static_cast<std::int64_t>(vitals.max_health));
      } else if (import.id == HostId::PumpkinPlayerGetFoodLevel) {
        push_i32(static_cast<std::uint32_t>(vitals.food_level));
        record_host_call("player_get_food_level", vitals.food_level);
      } else if (import.id == HostId::PumpkinPlayerGetSaturation) {
        push(make_f32(vitals.saturation));
        record_host_call("player_get_saturation", static_cast<std::int64_t>(vitals.saturation));
      } else {
        push(make_f32(vitals.exhaustion));
        record_host_call("player_get_exhaustion", static_cast<std::int64_t>(vitals.exhaustion));
      }
      return;
    }
    case HostId::PumpkinPlayerGetCameraEntityId: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.get-camera-entity-id used an invalid player handle " +
             std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.get-camera-entity-id used a stale player handle " +
             std::to_string(arguments[0]));
      std::int32_t entity_id = 0;
      if (!host_.player_camera_entity_id(player->second, &entity_id))
        trap("player.get-camera-entity-id has no live entity id for " + player->second);
      push_i32(static_cast<std::uint32_t>(entity_id));
      record_host_call("player_get_camera_entity_id", entity_id);
      return;
    }
    case HostId::PumpkinPlayerChat: {
      // Resource methods carry an implicit self handle before the message.
      // A chat is a delivery to a concrete player, so the handle must be live
      // and was issued by `server.get-player-by-name`; there is no guest-side
      // handle that can be fabricated to reach this path.
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.chat used an invalid player handle " + std::to_string(arguments[0]));
      const auto player = player_names_.find(arguments[0]);
      if (player == player_names_.end())
        trap("player.chat used a stale player handle " + std::to_string(arguments[0]));
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->messages, "", text);
      record_host_call("player_chat", static_cast<std::int64_t>(text.size()));
      host_.player_chat(text);
      return;
    }
    case HostId::PumpkinPlayerSendSystemMessage: {
      if (!valid_resource(arguments[0], ResourceKind::Player))
        trap("player.send-system-message used an invalid player handle " +
             std::to_string(arguments[0]));
      if (!valid_resource(arguments[1], ResourceKind::Text))
        trap("player.send-system-message used an invalid text handle " +
             std::to_string(arguments[1]));
      const auto player = player_names_.find(arguments[0]);
      const auto text = text_values_.find(arguments[1]);
      if (player == player_names_.end() || text == text_values_.end())
        trap("player.send-system-message used a stale player or text handle");
      const bool overlay = arguments[2] != 0;
      record(&report_->messages, "", text->second.plain());
      record_host_call("player_send_system_message",
                       static_cast<std::int64_t>(text->second.plain().size()));
      record_host_call("player_send_system_message_overlay", overlay ? 1 : 0);
      host_.send_system_message(player->second, text->second, overlay);
      return;
    }
    case HostId::PumpkinServerBroadcast: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("server.broadcast used an invalid server handle " + std::to_string(arguments[0]));
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->messages, "", text);
      const std::int32_t sent = host_.broadcast(text);
      record_host_call("broadcast", sent);
      return;
    }
    case HostId::PumpkinServerPlayer: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("server.get-player-by-name used an invalid server handle");
      const std::string name = host_string(arguments[1], arguments[2]);
      std::uint8_t* area = memory_at(arguments[3], 8);
      std::memset(area, 0, 8);
      std::string canonical;
      if (host_.find_player(name, &canonical)) {
        area[0] = 1;
        const std::uint32_t handle = new_player(std::move(canonical));
        std::memcpy(area + 4, &handle, sizeof(handle));
      }
      record_host_call("server_player_by_name", static_cast<std::int64_t>(name.size()));
      return;
    }
    case HostId::PumpkinTextText: {
      const std::string text = host_string(arguments[0], arguments[1]);
      record_host_call("text_text", static_cast<std::int64_t>(text.size()));
      TextComponent component;
      component.text = text;
      push_i32(new_text(std::move(component)));
      return;
    }
    case HostId::PumpkinTextAddText: {
      TextComponent& parent = text_at(arguments[0], "text-component.add-text");
      const std::string text = host_string(arguments[1], arguments[2]);
      TextComponent child;
      child.text = text;
      parent.children.push_back(std::move(child));
      record_host_call("text_add_text", static_cast<std::int64_t>(text.size()));
      return;
    }
    case HostId::PumpkinTextAddChild: {
      TextComponent& parent = text_at(arguments[0], "text-component.add-child");
      const TextComponent& child = text_at(arguments[1], "text-component.add-child");
      parent.children.push_back(child);
      record_host_call("text_add_child", static_cast<std::int64_t>(child.plain().size()));
      return;
    }
    case HostId::PumpkinTextColorNamed: {
      static constexpr const char* kNames[] = {
          "black", "dark_blue",    "dark_green", "dark_aqua", "dark_red", "dark_purple",
          "gold",  "gray",         "dark_gray",  "blue",      "green",    "aqua",
          "red",   "light_purple", "yellow",     "white",
      };
      if (arguments[1] >= 16)
        trap("text-component.color-named used an unknown named-color value");
      text_at(arguments[0], "text-component.color-named").color = kNames[arguments[1]];
      record_host_call("text_color_named", static_cast<std::int64_t>(arguments[1]));
      return;
    }
    case HostId::PumpkinTextColorRgb: {
      char color[8];
      std::snprintf(color, sizeof(color), "#%02x%02x%02x", arguments[1] & 0xff, arguments[2] & 0xff,
                    arguments[3] & 0xff);
      text_at(arguments[0], "text-component.color-rgb").color = color;
      record_host_call("text_color_rgb", static_cast<std::int64_t>(arguments[1]));
      return;
    }
    case HostId::PumpkinTextBold:
    case HostId::PumpkinTextItalic:
    case HostId::PumpkinTextUnderlined:
    case HostId::PumpkinTextStrikethrough:
    case HostId::PumpkinTextObfuscated: {
      const char* member = import.name.c_str();
      TextComponent& text = text_at(arguments[0], member);
      const bool value = arguments[1] != 0;
      const char* call = "text_bold";
      switch (import.id) {
      case HostId::PumpkinTextBold:
        text.has_bold = true;
        text.bold = value;
        break;
      case HostId::PumpkinTextItalic:
        text.has_italic = true;
        text.italic = value;
        call = "text_italic";
        break;
      case HostId::PumpkinTextUnderlined:
        text.has_underlined = true;
        text.underlined = value;
        call = "text_underlined";
        break;
      case HostId::PumpkinTextStrikethrough:
        text.has_strikethrough = true;
        text.strikethrough = value;
        call = "text_strikethrough";
        break;
      default:
        text.has_obfuscated = true;
        text.obfuscated = value;
        call = "text_obfuscated";
        break;
      }
      record_host_call(call, value ? 1 : 0);
      return;
    }
    case HostId::PumpkinTextInsertion:
    case HostId::PumpkinTextFont: {
      const std::string value = host_string(arguments[1], arguments[2]);
      TextComponent& text = text_at(arguments[0], import.name.c_str());
      const bool font = import.id == HostId::PumpkinTextFont;
      if (font)
        text.font = value;
      else
        text.insertion = value;
      record_host_call(font ? "text_font" : "text_insertion",
                       static_cast<std::int64_t>(value.size()));
      return;
    }
    case HostId::PumpkinTextClickOpenUrl:
    case HostId::PumpkinTextClickOpenFile:
    case HostId::PumpkinTextClickRunCommand:
    case HostId::PumpkinTextClickSuggestCommand:
    case HostId::PumpkinTextClickCopyToClipboard: {
      const std::string value = host_string(arguments[1], arguments[2]);
      TextComponent& text = text_at(arguments[0], import.name.c_str());
      const char* call = "text_click_open_url";
      switch (import.id) {
      case HostId::PumpkinTextClickOpenUrl:
        text.click_action = "open_url";
        break;
      case HostId::PumpkinTextClickOpenFile:
        text.click_action = "open_file";
        call = "text_click_open_file";
        break;
      case HostId::PumpkinTextClickRunCommand:
        text.click_action = "run_command";
        call = "text_click_run_command";
        break;
      case HostId::PumpkinTextClickSuggestCommand:
        text.click_action = "suggest_command";
        call = "text_click_suggest_command";
        break;
      default:
        text.click_action = "copy_to_clipboard";
        call = "text_click_copy_to_clipboard";
        break;
      }
      text.click_value = value;
      record_host_call(call, static_cast<std::int64_t>(value.size()));
      return;
    }
    case HostId::PumpkinTextClickChangePage: {
      TextComponent& text = text_at(arguments[0], "text-component.click-change-page");
      text.click_action = "change_page";
      text.click_value = std::to_string(arguments[1]);
      record_host_call("text_click_change_page", static_cast<std::int64_t>(arguments[1]));
      return;
    }
    case HostId::PumpkinTextHoverShowText: {
      TextComponent& text = text_at(arguments[0], "text-component.hover-show-text");
      const TextComponent& hover = text_at(arguments[1], "text-component.hover-show-text");
      text.hover_text = std::make_shared<TextComponent>(hover);
      record_host_call("text_hover_show_text", static_cast<std::int64_t>(hover.plain().size()));
      return;
    }
    case HostId::Reply:
    case HostId::PlayerMessage:
    case HostId::Broadcast: {
      const std::string text = host_string(arguments[0], arguments[1]);
      record(&report_->messages, "", text);
      host_.message(text);
      return;
    }
    case HostId::PumpkinLog: {
      // Pumpkin's `log(level, message)`: the level is metadata the server
      // prefix can carry, the message is the UTF-8 pair. Record the level too,
      // so a report can observe the severity the component passed.
      const std::int32_t level = static_cast<std::int32_t>(arguments[0]);
      const std::string text = host_string(arguments[1], arguments[2]);
      record(&report_->log, "log: ", text);
      record_host_call("log_level", level);
      host_.log_level(level, text);
      return;
    }
    case HostId::PumpkinContextServer: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.get-server used an invalid context handle " + std::to_string(arguments[0]));
      push_i32(new_server());
      return;
    }
    case HostId::PumpkinContextRegisterEvent: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.register-event used an invalid context handle " +
             std::to_string(arguments[0]));
      // handler-id, event-type, priority, blocking. The registration is real:
      // the tick dispatcher consults it before calling handle-event.
      event_handlers_[arguments[2]] = arguments[1];
      record_host_call("register_event", static_cast<std::int64_t>(arguments[1]));
      return;
    }
    case HostId::PumpkinCommandNew: {
      const std::uint32_t names_pointer = arguments[0];
      const std::uint32_t names_count = arguments[1];
      if (names_count == 0)
        trap("command constructor needs at least one name");
      if (names_count > 1024)
        trap("command constructor received too many names");
      CommandValue command;
      for (std::uint32_t i = 0; i < names_count; ++i) {
        command.names.push_back(read_string_pair(names_pointer + i * 8));
        if (command.names.back().empty())
          trap("command constructor received an empty name");
      }
      command.description = host_string(arguments[2], arguments[3]);
      const std::uint32_t handle = new_command(std::move(command));
      record_host_call("command_new", names_count);
      push_i32(handle);
      return;
    }
    case HostId::PumpkinCommandExecute: {
      CommandValue& command = command_at(arguments[0], "command.execute-with-handler-id");
      command.handler_id = arguments[1];
      command.has_handler = true;
      record_host_call("command_execute", arguments[1]);
      return;
    }
    case HostId::PumpkinCommandThen: {
      CommandValue& command = command_at(arguments[0], "command.then");
      command_node_at(arguments[1], "command.then");
      command.nodes.push_back(arguments[1]);
      record_host_call("command_then", 1);
      return;
    }
    case HostId::PumpkinCommandNodeArgument: {
      const std::string name = host_string(arguments[0], arguments[1]);
      if (name.empty())
        trap("command-node.argument received an empty name");
      if (name.size() > 1024)
        trap("command-node.argument received an overlong name");
      static constexpr const char* kArgumentTypes[] = {
          "bool",          "float",          "double",         "integer",
          "long",          "string",         "entities",       "entity",
          "players",       "game-profile",   "block-pos",      "column-pos",
          "position3d",    "position2d",     "block-state",    "block-predicate",
          "item",          "item-predicate", "color",          "component",
          "style",         "message",        "nbt-compound-tag", "nbt-tag",
          "nbt-path",      "objective",      "objective-criteria", "operation",
          "particle",      "angle",          "rotation",       "scoreboard-slot",
          "score-holder",  "swizzle",        "team",           "item-slot",
          "resource-location", "mob-effect", "function",       "entity-anchor",
          "int-range",     "float-range",    "dimension",      "gamemode",
          "difficulty",    "time",           "resource",       "resource-or-tag",
          "resource-or-tag-key", "resource-key", "template-mirror", "template-rotation",
          "uuid",
      };
      const std::uint32_t argument_case = arguments[2];
      if (argument_case >= std::size(kArgumentTypes))
        trap("command-node.argument received an unknown argument case " +
             std::to_string(argument_case));
      CommandNodeValue node;
      node.name = name;
      node.is_argument = true;
      node.argument.type = kArgumentTypes[argument_case];
      auto present = [](std::uint32_t discriminant) {
        if (discriminant > 1)
          trap("command-node.argument received an invalid option discriminant");
        return discriminant != 0;
      };
      auto f32_at = [](std::uint64_t bits) {
        const std::uint32_t low = static_cast<std::uint32_t>(bits);
        float value = 0.0f;
        std::memcpy(&value, &low, sizeof(value));
        return value;
      };
      auto f64_at = [](std::uint64_t bits) {
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
      };
      switch (argument_case) {
      case 1: // float(tuple<option<f32>, option<f32>>)
        if (present(arguments[3]))
          node.argument.min_float = f32_at(argument_bits[4]);
        if (present(arguments[5]))
          node.argument.max_float = f32_at(argument_bits[6]);
        break;
      case 2: // double(tuple<option<f64>, option<f64>>)
        if (present(arguments[3]))
          node.argument.min_double = f64_at(argument_bits[4]);
        if (present(arguments[5]))
          node.argument.max_double = f64_at(argument_bits[6]);
        break;
      case 3: // integer(tuple<option<s32>, option<s32>>)
        if (present(arguments[3]))
          node.argument.min_integer = static_cast<std::int32_t>(argument_bits[4]);
        if (present(arguments[5]))
          node.argument.max_integer = static_cast<std::int32_t>(argument_bits[6]);
        break;
      case 4: // long(tuple<option<s64>, option<s64>>)
        if (present(arguments[3]))
          node.argument.min_long = static_cast<std::int64_t>(argument_bits[4]);
        if (present(arguments[5]))
          node.argument.max_long = static_cast<std::int64_t>(argument_bits[6]);
        break;
      case 5: // string(string-type)
        if (arguments[3] > 2)
          trap("command-node.argument received an unknown string-type");
        node.argument.string_type = static_cast<std::int32_t>(arguments[3]);
        break;
      case 32: // score-holder(bool)
        if (arguments[3] > 1)
          trap("command-node.argument received an invalid score-holder flag");
        node.argument.score_holder_multiple = arguments[3] != 0;
        break;
      case 45: // time(option<s32>)
        if (present(arguments[3]))
          node.argument.time_min = static_cast<std::int32_t>(argument_bits[4]);
        break;
      case 46: // resource(string)
      case 47: // resource-or-tag(string)
      case 48: // resource-or-tag-key(string)
      case 49: // resource-key(string)
        if ((argument_bits[4] >> 32) != 0)
          trap("command-node.argument received an overlong resource string");
        node.argument.resource =
            host_string(arguments[3], static_cast<std::uint32_t>(argument_bits[4]));
        break;
      default:
        break;
      }
      const std::uint32_t handle = new_command_node(std::move(node));
      record_host_call("command_node_argument", argument_case);
      if (argument_case == 1) {
        if (command_node_at(handle, "command-node.argument").argument.min_float)
          record_host_call("command_node_argument_min",
                           *command_node_at(handle, "command-node.argument").argument.min_float);
        if (command_node_at(handle, "command-node.argument").argument.max_float)
          record_host_call("command_node_argument_max",
                           *command_node_at(handle, "command-node.argument").argument.max_float);
      } else if (argument_case == 2) {
        if (command_node_at(handle, "command-node.argument").argument.min_double)
          record_host_call("command_node_argument_min",
                           *command_node_at(handle, "command-node.argument").argument.min_double);
        if (command_node_at(handle, "command-node.argument").argument.max_double)
          record_host_call("command_node_argument_max",
                           *command_node_at(handle, "command-node.argument").argument.max_double);
      } else if (argument_case == 3) {
        if (command_node_at(handle, "command-node.argument").argument.min_integer)
          record_host_call("command_node_argument_min",
                           *command_node_at(handle, "command-node.argument").argument.min_integer);
        if (command_node_at(handle, "command-node.argument").argument.max_integer)
          record_host_call("command_node_argument_max",
                           *command_node_at(handle, "command-node.argument").argument.max_integer);
      } else if (argument_case == 4) {
        if (command_node_at(handle, "command-node.argument").argument.min_long)
          record_host_call("command_node_argument_min",
                           *command_node_at(handle, "command-node.argument").argument.min_long);
        if (command_node_at(handle, "command-node.argument").argument.max_long)
          record_host_call("command_node_argument_max",
                           *command_node_at(handle, "command-node.argument").argument.max_long);
      } else if (argument_case == 5) {
        record_host_call("command_node_argument_string", arguments[3]);
      } else if (argument_case == 45 &&
                 command_node_at(handle, "command-node.argument").argument.time_min) {
        record_host_call("command_node_argument_min",
                         *command_node_at(handle, "command-node.argument").argument.time_min);
      }
      push_i32(handle);
      return;
    }
    case HostId::PumpkinCommandNodeLiteral: {
      const std::string name = host_string(arguments[0], arguments[1]);
      if (name.empty())
        trap("command-node.literal received an empty name");
      if (name.size() > 1024)
        trap("command-node.literal received an overlong name");
      CommandNodeValue node;
      node.name = name;
      const std::uint32_t handle = new_command_node(std::move(node));
      record_host_call("command_node_literal", static_cast<std::int64_t>(name.size()));
      push_i32(handle);
      return;
    }
    case HostId::PumpkinCommandNodeThen: {
      if (arguments[0] == arguments[1])
        trap("command-node.then cannot attach a node to itself");
      CommandNodeValue& parent = command_node_at(arguments[0], "command-node.then");
      command_node_at(arguments[1], "command-node.then");
      parent.children.push_back(arguments[1]);
      record_host_call("command_node_then", 1);
      return;
    }
    case HostId::PumpkinCommandNodeExecute: {
      CommandNodeValue& node =
          command_node_at(arguments[0], "command-node.execute-with-handler-id");
      node.execute_handler = arguments[1];
      node.has_execute = true;
      record_host_call("command_node_execute", arguments[1]);
      return;
    }
    case HostId::PumpkinCommandNodeSuggest: {
      CommandNodeValue& node =
          command_node_at(arguments[0], "command-node.suggest-with-handler-id");
      node.suggest_handler = arguments[1];
      node.has_suggest = true;
      record_host_call("command_node_suggest", arguments[1]);
      return;
    }
    case HostId::PumpkinCommandNodeRequire: {
      CommandNodeValue& node =
          command_node_at(arguments[0], "command-node.require-with-handler-id");
      node.require_handler = arguments[1];
      node.has_require = true;
      record_host_call("command_node_require", arguments[1]);
      return;
    }
    case HostId::PumpkinContextRegisterCommand: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.register-command used an invalid context handle " +
             std::to_string(arguments[0]));
      const CommandValue& command = command_at(arguments[1], "context.register-command");
      if (!command.has_handler)
        trap("context.register-command used a command with no handler id");
      CommandRegistration registration;
      registration.name = command.names.front();
      registration.aliases.assign(command.names.begin() + 1, command.names.end());
      registration.description = command.description;
      registration.permission = host_string(arguments[2], arguments[3]);
      registration.handler_id = command.handler_id;
      std::vector<std::uint32_t> ancestry;
      std::uint32_t node_count = 0;
      registration.nodes.reserve(command.nodes.size());
      for (const std::uint32_t handle : command.nodes)
        registration.nodes.push_back(
            command_node_registration(handle, &ancestry, &node_count, 0));
      host_.register_command(registration);
      record_host_call("register_command", static_cast<std::int64_t>(registration.name.size()));
      return;
    }
    case HostId::PumpkinContextRegisterPermission: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.register-permission used an invalid context handle " +
             std::to_string(arguments[0]));
      PermissionGrant permission;
      permission.node = host_string(arguments[1], arguments[2]);
      permission.description = host_string(arguments[3], arguments[4]);
      permission.default_case = arguments[5];
      permission.default_level = arguments[6];
      const std::uint32_t children = arguments[7];
      const std::uint32_t count = arguments[8];
      if (count > 4096)
        trap("context.register-permission received too many children");
      for (std::uint32_t i = 0; i < count; ++i) {
        PermissionChild child;
        child.node = read_string_pair(children + i * 12);
        child.value = memory_at(children + i * 12 + 8, 1)[0] != 0;
        permission.children.push_back(std::move(child));
      }
      std::string error;
      const bool ok = host_.register_permission(permission, &error);
      std::uint8_t* area = memory_at(arguments[9], 12);
      std::memset(area, 0, 12);
      if (!ok) {
        area[0] = 1;
        write_string_pair(import, arguments[9] + 4, error);
      }
      record_host_call("register_permission", ok ? 1 : 0);
      return;
    }
    case HostId::PumpkinContextGetDataFolder: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.get-data-folder used an invalid context handle " +
             std::to_string(arguments[0]));
      const std::string folder = host_.data_folder();
      write_string_pair(import, arguments[1], folder);
      record_host_call("get_data_folder", static_cast<std::int64_t>(folder.size()));
      return;
    }
    case HostId::PumpkinContextMarketplaceMetadata: {
      if (!valid_resource(arguments[0], ResourceKind::Context))
        trap("context.get-marketplace-metadata used an invalid context handle " +
             std::to_string(arguments[0]));
      std::uint8_t* area = memory_at(arguments[1], 96);
      std::memset(area, 0, 96);
      MarketplaceMetadata metadata;
      const bool present = host_.marketplace_metadata(&metadata);
      if (present) {
        area[0] = 1;
        write_marketplace_metadata(import, arguments[1] + 8, metadata);
      }
      record_host_call("marketplace_metadata", present ? 1 : 0);
      return;
    }
    case HostId::PumpkinServerWorld: {
      // `server.get-world-by-name`: arguments are the server handle, the
      // name's (pointer, length) pair, and the guest's 8-byte return area.
      // The host owns the world identity; the guest only sees a typed handle.
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("server.get-world-by-name used an invalid server handle");
      const std::string name = host_string(arguments[1], arguments[2]);
      std::uint8_t* area = memory_at(arguments[3], 8);
      std::memset(area, 0, 8);
      if (host_.has_world(name)) {
        area[0] = 1;
        const std::uint32_t handle = new_world(name);
        std::memcpy(area + 4, &handle, sizeof(handle));
      }
      record_host_call("server_world_by_name", static_cast<std::int64_t>(name.size()));
      return;
    }
    case HostId::PumpkinWorldGetBlock: {
      // `world.get-block-state-id`: the leading i32 is the world handle, the
      // three coordinates follow it.
      if (!valid_resource(arguments[0], ResourceKind::World))
        trap("world.get-block-state-id used an invalid world handle");
      const std::int32_t x = static_cast<std::int32_t>(arguments[1]);
      const std::int32_t y = static_cast<std::int32_t>(arguments[2]);
      const std::int32_t z = static_cast<std::int32_t>(arguments[3]);
      const std::int32_t value = host_.world_get_block(x, y, z);
      record_host_call("world_get_block", value);
      record_host_position("world_get_block_at", x, y, z);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::PlayerCount: {
      if (count == 1 && !valid_resource(arguments[0], ResourceKind::Server))
        trap("server.get-player-count used an invalid server handle");
      const std::int32_t value = host_.player_count();
      record_host_call("player_count", value);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::PumpkinServerAllPlayers: {
      if (count != 2 || !valid_resource(arguments[0], ResourceKind::Server))
        trap("server.get-all-players used an invalid server handle");
      std::vector<std::string> names;
      host_.all_players(&names);
      std::vector<std::uint32_t> handles;
      handles.reserve(names.size());
      for (std::string& name : names)
        handles.push_back(new_player(std::move(name)));
      write_handle_list(import, arguments[1], handles);
      record_host_call("server_all_players", static_cast<std::int64_t>(handles.size()));
      return;
    }
    case HostId::PumpkinServerPlayerCountInWorld: {
      if (count != 2 || !valid_resource(arguments[0], ResourceKind::Server) ||
          !valid_resource(arguments[1], ResourceKind::World))
        trap("server.get-player-count-in-world used an invalid server or world handle");
      const auto world = world_names_.find(arguments[1]);
      if (world == world_names_.end())
        trap("server.get-player-count-in-world used a stale world handle");
      const std::int32_t value = host_.player_count_in_world(world->second);
      record_host_call("server_player_count_in_world", value);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::PumpkinServerPlayersInWorld: {
      if (count != 3 || !valid_resource(arguments[0], ResourceKind::Server) ||
          !valid_resource(arguments[1], ResourceKind::World))
        trap("server.get-players-in-world used an invalid server or world handle");
      const auto world = world_names_.find(arguments[1]);
      if (world == world_names_.end())
        trap("server.get-players-in-world used a stale world handle");
      std::vector<std::string> names;
      host_.players_in_world(world->second, &names);
      std::vector<std::uint32_t> handles;
      handles.reserve(names.size());
      for (std::string& name : names)
        handles.push_back(new_player(std::move(name)));
      write_handle_list(import, arguments[2], handles);
      record_host_call("server_players_in_world", static_cast<std::int64_t>(handles.size()));
      return;
    }
    case HostId::WorldGetBlock: {
      const std::int32_t x = static_cast<std::int32_t>(arguments[0]);
      const std::int32_t y = static_cast<std::int32_t>(arguments[1]);
      const std::int32_t z = static_cast<std::int32_t>(arguments[2]);
      const std::int32_t value = host_.world_get_block(x, y, z);
      record_host_call("world_get_block", value);
      record_host_position("world_get_block_at", x, y, z);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::PumpkinServerHasWorld: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("server.has-world used an invalid server handle");
      const std::string name = host_string(arguments[1], arguments[2]);
      const bool found = host_.has_world(name);
      record_host_call("server_has_world", found ? 1 : 0);
      push_i32(found ? 1u : 0u);
      return;
    }
    case HostId::PumpkinServerMaxPlayers:
    case HostId::PumpkinServerViewDistance:
    case HostId::PumpkinServerSimulationDistance:
    case HostId::PumpkinServerDefaultGamemode:
    case HostId::PumpkinServerDifficulty:
    case HostId::PumpkinServerOnlineMode:
    case HostId::PumpkinServerWhitelistEnabled: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("a server setting was read through an invalid server handle");
      const char* call = "server_max_players";
      std::int32_t value = 0;
      switch (import.id) {
      case HostId::PumpkinServerMaxPlayers:
        value = host_.server_max_players();
        break;
      case HostId::PumpkinServerViewDistance:
        value = host_.server_view_distance();
        call = "server_view_distance";
        break;
      case HostId::PumpkinServerSimulationDistance:
        value = host_.server_simulation_distance();
        call = "server_simulation_distance";
        break;
      case HostId::PumpkinServerDefaultGamemode:
        value = host_.server_default_gamemode();
        call = "server_default_gamemode";
        break;
      case HostId::PumpkinServerDifficulty:
        value = host_.server_difficulty();
        call = "server_difficulty";
        break;
      case HostId::PumpkinServerOnlineMode:
        value = host_.server_online_mode() ? 1 : 0;
        call = "server_online_mode";
        break;
      case HostId::PumpkinServerWhitelistEnabled:
        value = host_.server_whitelist_enabled() ? 1 : 0;
        call = "server_whitelist_enabled";
        break;
      default:
        break;
      }
      record_host_call(call, value);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::PumpkinServerMotd: {
      if (!valid_resource(arguments[0], ResourceKind::Server))
        trap("server.get-motd used an invalid server handle");
      const std::string motd = host_.server_motd();
      write_string_pair(import, arguments[1], motd);
      record_host_call("server_motd", static_cast<std::int64_t>(motd.size()));
      return;
    }
    case HostId::WorldGetBlockId: {
      // `world.get-block-id`: the leading i32 is the world handle, then the
      // three coordinates, matching `get-block-state-id`'s resource layout.
      if (!valid_resource(arguments[0], ResourceKind::World))
        trap("world.get-block-id used an invalid world handle");
      const std::int32_t x = static_cast<std::int32_t>(arguments[1]);
      const std::int32_t y = static_cast<std::int32_t>(arguments[2]);
      const std::int32_t z = static_cast<std::int32_t>(arguments[3]);
      BlockInfo block;
      if (!host_.world_get_block_info(x, y, z, &block))
        block = BlockInfo{};
      record_host_call("world_get_block_id", block.id);
      record_host_position("world_get_block_id_at", x, y, z);
      push_i32(block.id);
      return;
    }
    case HostId::WorldSetTime: {
      if (!valid_resource(arguments[0], ResourceKind::World))
        trap("world.set-time-of-day used an invalid world handle");
      const std::int64_t ticks = static_cast<std::int64_t>(argument_bits[1]);
      const bool applied = host_.world_set_time(ticks);
      record_host_call("world_set_time", applied ? 1 : 0);
      push_i32(applied ? 1u : 0u);
      return;
    }
    case HostId::WorldGetBlockInfo: {
      BlockInfo block;
      if (!host_.world_get_block_info(static_cast<std::int32_t>(arguments[0]),
                                      static_cast<std::int32_t>(arguments[1]),
                                      static_cast<std::int32_t>(arguments[2]), &block)) {
        // A host without a block registry can still carry the result shape: the
        // position's state is air, so the record names air and carries air's
        // single state id rather than a fabricated definition.
        block = BlockInfo{};
        block.name = "minecraft:air";
        block.state_ids = {0};
        block.is_solid = false;
        block.is_air = true;
      }
      const std::uint32_t area = allocate_block(import, block);
      record_host_call("world_get_block_info", block.id);
      push_i32(area);
      return;
    }
    case HostId::WorldSetBlock:
      push_i32(static_cast<std::uint32_t>(host_.world_set_block(
          static_cast<std::int32_t>(arguments[0]), static_cast<std::int32_t>(arguments[1]),
          static_cast<std::int32_t>(arguments[2]), static_cast<std::int32_t>(arguments[3]))));
      return;
    case HostId::PumpkinWorldSetBlock: {
      const std::int32_t x = static_cast<std::int32_t>(arguments[0]);
      const std::int32_t y = static_cast<std::int32_t>(arguments[1]);
      const std::int32_t z = static_cast<std::int32_t>(arguments[2]);
      const std::uint8_t* block = memory_at(arguments[3], 56);
      const std::uint16_t state = static_cast<std::uint16_t>(block[38] | (block[39] << 8));
      const std::int32_t result = host_.world_set_block(x, y, z, state);
      record_host_call("world_set_block", result);
      return;
    }
    case HostId::PumpkinWorldSetBlockAsync:
      trap("the async world.set-block lower reached the synchronous host path");
    case HostId::PumpkinWorldSetBlockSpill: {
      // The compiler's async lower spills the method's parameter tuple behind
      // one in-pointer: self at 0, block-pos at 4, block at 16, flags at 72.
      const std::uint8_t* area = memory_at(arguments[0], 76);
      const std::int32_t x =
          static_cast<std::int32_t>(area[4] | (area[5] << 8) | (area[6] << 16) | (area[7] << 24));
      const std::int32_t y =
          static_cast<std::int32_t>(area[8] | (area[9] << 8) | (area[10] << 16) | (area[11] << 24));
      const std::int32_t z = static_cast<std::int32_t>(area[12] | (area[13] << 8) |
                                                       (area[14] << 16) | (area[15] << 24));
      const std::uint16_t state = static_cast<std::uint16_t>(area[16 + 38] | (area[16 + 39] << 8));
      const std::int32_t result = host_.world_set_block(x, y, z, state);
      record_host_call("world_set_block", result);
      return;
    }
    case HostId::WorldTime: {
      if (count == 1 && !valid_resource(arguments[0], ResourceKind::World))
        trap("world.get-time-of-day used an invalid world handle");
      const std::int64_t value = host_.world_time();
      record_host_call("world_time", value);
      push(make_i64(static_cast<std::uint64_t>(value)));
      return;
    }
    case HostId::WorldWeather: {
      if (count == 1 && !valid_resource(arguments[0], ResourceKind::World))
        trap("world weather used an invalid world handle");
      const std::int32_t value = host_.world_weather();
      record_host_call("world_weather", value);
      push_i32(static_cast<std::uint32_t>(value));
      return;
    }
    case HostId::WorldIsRaining: {
      if (count == 1 && !valid_resource(arguments[0], ResourceKind::World))
        trap("world.is-raining used an invalid world handle");
      const bool value = host_.world_weather() != 0;
      record_host_call("world_is_raining", value ? 1 : 0);
      push_i32(value ? 1u : 0u);
      return;
    }
    case HostId::WorldIsThundering: {
      if (count == 1 && !valid_resource(arguments[0], ResourceKind::World))
        trap("world.is-thundering used an invalid world handle");
      const bool value = host_.world_weather() == 2;
      record_host_call("world_is_thundering", value ? 1 : 0);
      push_i32(value ? 1u : 0u);
      return;
    }
    case HostId::EntityFind: {
      const std::string name = host_string(arguments[0], arguments[1]);
      push_i32(static_cast<std::uint32_t>(host_.entity_find(name)));
      return;
    }
    case HostId::ConfigGet: {
      const std::string key = host_string(arguments[0], arguments[1]);
      std::string value;
      const std::int32_t found = host_.config_get(key, &value);
      if (found < 0) {
        push_i32(static_cast<std::uint32_t>(-1));
        return;
      }
      const std::uint32_t capacity = arguments[3];
      if (value.size() > capacity) {
        push_i32(static_cast<std::uint32_t>(-2));
        return;
      }
      if (!value.empty()) {
        std::uint8_t* destination = memory_at(arguments[2], value.size());
        std::memcpy(destination, value.data(), value.size());
      }
      host_bytes_ += value.size();
      if (host_bytes_ > limits_.max_host_bytes)
        budget("host message budget exhausted (" + std::to_string(limits_.max_host_bytes) +
               " bytes)");
      push_i32(static_cast<std::uint32_t>(value.size()));
      return;
    }
    case HostId::ConfigGetString: {
      const std::string key = host_string(arguments[0], arguments[1]);
      std::string value;
      if (host_.config_get(key, &value) < 0)
        value.clear();
      const std::uint32_t area = allocate_string(import, value);
      record_host_call("config_get_string", static_cast<std::int64_t>(value.size()));
      push_i32(area);
      return;
    }
    case HostId::ConfigPair: {
      const std::string key = host_string(arguments[0], arguments[1]);
      std::string stored_key;
      std::string value;
      if (host_.config_pair(key, &stored_key, &value) < 0)
        value.clear();
      const std::uint32_t area = allocate_string_record(import, stored_key, value);
      record_host_call("config_pair", static_cast<std::int64_t>(value.size()));
      push_i32(area);
      return;
    }
    case HostId::ConfigKeys: {
      std::vector<std::string> keys;
      if (host_.config_keys(&keys) < 0)
        keys.clear();
      const std::uint32_t area = allocate_string_list(import, keys);
      record_host_call("config_keys", static_cast<std::int64_t>(keys.size()));
      push_i32(area);
      return;
    }
    case HostId::ConfigSet: {
      const std::string key = host_string(arguments[0], arguments[1]);
      const std::string value = host_string(arguments[2], arguments[3]);
      push_i32(static_cast<std::uint32_t>(host_.config_set(key, value)));
      return;
    }
    case HostId::SchedulerScheduleDelayed: {
      const std::uint32_t task = schedule_task(arguments[0], argument_bits[1], 0, false);
      record_host_call("scheduler_schedule_delayed", task);
      push_i32(task);
      return;
    }
    case HostId::SchedulerScheduleRepeating: {
      const std::uint32_t task =
          schedule_task(arguments[0], argument_bits[1], argument_bits[2], true);
      record_host_call("scheduler_schedule_repeating", task);
      push_i32(task);
      return;
    }
    case HostId::SchedulerCancel:
      cancel_task(arguments[0]);
      record_host_call("scheduler_cancel", arguments[0]);
      return;
    case HostId::UuidGenerate: {
      const UuidValue id = uuid_generate_value();
      std::uint8_t* area = memory_at(arguments[0], 16);
      std::memset(area, 0, 16);
      write_u64(arguments[0], id.high);
      write_u64(arguments[0] + 8, id.low);
      record_host_call("uuid_generate", 1);
      return;
    }
    case HostId::UuidParse: {
      const std::string text = host_string(arguments[0], arguments[1]);
      std::uint8_t* area = memory_at(arguments[2], 24);
      std::memset(area, 0, 24);
      UuidValue id;
      const bool parsed = uuid_parse_text(text, &id);
      if (parsed) {
        area[0] = 1;
        write_u64(arguments[2] + 8, id.high);
        write_u64(arguments[2] + 16, id.low);
      }
      record_host_call("uuid_parse", parsed ? 1 : 0);
      return;
    }
    case HostId::UuidToString: {
      const UuidValue id{argument_bits[0], argument_bits[1]};
      const std::string text = uuid_format(id);
      write_string_pair(import, arguments[2], text);
      record_host_call("uuid_to_string", static_cast<std::int64_t>(text.size()));
      return;
    }
    case HostId::I18nTranslate: {
      const std::string key = host_string(arguments[0], arguments[1]);
      const std::int32_t locale = static_cast<std::int32_t>(arguments[2]);
      const std::string* found = nullptr;
      const auto exact = translations_.find(locale);
      if (exact != translations_.end()) {
        const auto entry = exact->second.find(key);
        if (entry != exact->second.end())
          found = &entry->second;
      }
      if (found == nullptr && locale != kEnglishLocale) {
        const auto english = translations_.find(kEnglishLocale);
        if (english != translations_.end()) {
          const auto entry = english->second.find(key);
          if (entry != english->second.end())
            found = &entry->second;
        }
      }
      const std::string translated = found != nullptr ? *found : key;
      write_string_pair(import, arguments[3], translated);
      record_host_call("i18n_translate", static_cast<std::int64_t>(translated.size()));
      return;
    }
    case HostId::I18nLoadTranslations: {
      const std::string ns = host_string(arguments[0], arguments[1]);
      const std::string json = host_string(arguments[2], arguments[3]);
      const std::int32_t locale = static_cast<std::int32_t>(arguments[4]);
      std::map<std::string, std::string> parsed;
      if (!parse_flat_json(json, &parsed))
        trap("i18n.load-translations received malformed flat JSON");
      auto& target = translations_[locale];
      for (const auto& entry : parsed) {
        target[entry.first] = entry.second;
        if (!ns.empty())
          target[ns + ":" + entry.first] = entry.second;
      }
      record_host_call("i18n_load_translations", static_cast<std::int64_t>(parsed.size()));
      return;
    }
    case HostId::ItemStackNew: {
      const std::string key = host_string(arguments[0], arguments[1]);
      const std::int32_t max_count = host_.item_max_count(key);
      if (max_count <= 0)
        trap("item-stack constructor used an unknown item registry key " + key);
      const std::uint8_t count = static_cast<std::uint8_t>(arguments[2] & 0xff);
      const std::uint8_t clamped = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(count, static_cast<std::uint32_t>(max_count)));
      ItemStackValue item;
      item.registry_key = key;
      item.count = clamped;
      item.max_count = max_count;
      item.max_damage = host_.item_max_damage(key);
      item.damageable = host_.item_damageable(key);
      const std::uint32_t handle = new_item(std::move(item));
      record_host_call("item_stack_new", clamped);
      push_i32(handle);
      return;
    }
    case HostId::ItemStackGetRegistryKey: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-registry-key");
      write_string_pair(import, arguments[1], item.registry_key);
      record_host_call("item_stack_get_registry_key",
                       static_cast<std::int64_t>(item.registry_key.size()));
      return;
    }
    case HostId::ItemStackGetCount: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-count");
      record_host_call("item_stack_get_count", item.count);
      push_i32(item.count);
      return;
    }
    case HostId::ItemStackSetCount: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-count");
      item.count = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(arguments[1] & 0xff, static_cast<std::uint32_t>(item.max_count)));
      record_host_call("item_stack_set_count", item.count);
      return;
    }
    case HostId::ItemStackGetMaxCount: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-max-count");
      record_host_call("item_stack_get_max_count", item.max_count);
      push_i32(static_cast<std::uint32_t>(item.max_count));
      return;
    }
    case HostId::ItemStackClone: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.clone");
      ItemStackValue copy = item;
      const std::uint32_t handle = new_item(std::move(copy));
      record_host_call("item_stack_clone", item.count);
      push_i32(handle);
      return;
    }
    case HostId::ItemStackWithCount: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.with-count");
      ItemStackValue copy = item;
      copy.count = static_cast<std::uint8_t>(
          std::min<std::uint32_t>(arguments[1] & 0xff, static_cast<std::uint32_t>(item.max_count)));
      const std::uint8_t copied_count = copy.count;
      const std::uint32_t handle = new_item(std::move(copy));
      record_host_call("item_stack_with_count", copied_count);
      push_i32(handle);
      return;
    }
    case HostId::ItemStackIsSimilar: {
      const ItemStackValue& left = item_at(arguments[0], "item-stack.is-similar");
      const ItemStackValue& right = item_at(arguments[1], "item-stack.is-similar");
      const bool similar = same_item_data(left, right);
      record_host_call("item_stack_is_similar", similar ? 1 : 0);
      push_i32(similar ? 1u : 0u);
      return;
    }
    case HostId::ItemStackGetDamage: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-damage");
      record_host_call("item_stack_get_damage", item.damage);
      push_i32(static_cast<std::uint32_t>(item.damage));
      return;
    }
    case HostId::ItemStackSetDamage: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-damage");
      const std::int32_t value = static_cast<std::int32_t>(arguments[1]);
      item.damage = item.damageable ? std::max(0, std::min(value, item.max_damage)) : 0;
      record_host_call("item_stack_set_damage", item.damage);
      return;
    }
    case HostId::ItemStackGetMaxDamage: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-max-damage");
      record_host_call("item_stack_get_max_damage", item.max_damage);
      push_i32(static_cast<std::uint32_t>(item.max_damage));
      return;
    }
    case HostId::ItemStackIsDamageable: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.is-damageable");
      record_host_call("item_stack_is_damageable", item.damageable ? 1 : 0);
      push_i32(item.damageable ? 1u : 0u);
      return;
    }
    case HostId::ItemStackIsUnbreakable: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.is-unbreakable");
      record_host_call("item_stack_is_unbreakable", item.unbreakable ? 1 : 0);
      push_i32(item.unbreakable ? 1u : 0u);
      return;
    }
    case HostId::ItemStackSetUnbreakable: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-unbreakable");
      item.unbreakable = arguments[1] != 0;
      record_host_call("item_stack_set_unbreakable", item.unbreakable ? 1 : 0);
      return;
    }
    case HostId::ItemStackGetRepairCost: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-repair-cost");
      record_host_call("item_stack_get_repair_cost", item.repair_cost);
      push_i32(static_cast<std::uint32_t>(item.repair_cost));
      return;
    }
    case HostId::ItemStackSetRepairCost: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-repair-cost");
      item.repair_cost = static_cast<std::int32_t>(arguments[1]);
      record_host_call("item_stack_set_repair_cost", item.repair_cost);
      return;
    }
    case HostId::ItemStackGetItemModel: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-item-model");
      std::uint8_t* option = memory_at(arguments[1], 12);
      std::memset(option, 0, 12);
      if (item.item_model) {
        option[0] = 1;
        write_string_pair(import, arguments[1] + 4, *item.item_model);
      }
      record_host_call("item_stack_get_item_model", item.item_model ? 1 : 0);
      return;
    }
    case HostId::ItemStackSetItemModel: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-item-model");
      if (arguments[1] != 0)
        item.item_model = host_string(arguments[2], arguments[3]);
      else
        item.item_model.reset();
      record_host_call("item_stack_set_item_model", arguments[1] != 0 ? 1 : 0);
      return;
    }
    case HostId::ItemStackGetEnchantmentGlintOverride: {
      const ItemStackValue& item =
          item_at(arguments[0], "item-stack.get-enchantment-glint-override");
      write_option_bool(arguments[1], item.glint_override);
      record_host_call("item_stack_get_enchantment_glint_override",
                       item.glint_override ? (*item.glint_override ? 1 : 0) : -1);
      return;
    }
    case HostId::ItemStackSetEnchantmentGlintOverride: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-enchantment-glint-override");
      if (arguments[1] != 0)
        item.glint_override = arguments[2] != 0;
      else
        item.glint_override.reset();
      record_host_call("item_stack_set_enchantment_glint_override",
                       arguments[1] != 0 ? (arguments[2] != 0 ? 1 : 0) : -1);
      return;
    }
    case HostId::ItemStackGetAttributeModifiers: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-attribute-modifiers");
      write_attribute_modifier_list(import, arguments[1], item.attribute_modifiers);
      record_host_call("item_stack_get_attribute_modifiers",
                       static_cast<std::int64_t>(item.attribute_modifiers.size()));
      return;
    }
    case HostId::ItemStackAddAttributeModifier: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.add-attribute-modifier");
      ItemAttributeModifierValue modifier;
      modifier.attribute = arguments[1];
      modifier.id = host_string(arguments[2], arguments[3]);
      std::memcpy(&modifier.amount, &argument_bits[4], sizeof(modifier.amount));
      modifier.operation = arguments[5];
      modifier.slot = arguments[6];
      item.attribute_modifiers.push_back(std::move(modifier));
      record_host_call("item_stack_add_attribute_modifier",
                       static_cast<std::int64_t>(item.attribute_modifiers.size()));
      return;
    }
    case HostId::ItemStackRemoveAttributeModifiers: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.remove-attribute-modifiers");
      const std::uint32_t attribute = arguments[1];
      const auto end =
          std::remove_if(item.attribute_modifiers.begin(), item.attribute_modifiers.end(),
                         [attribute](const ItemAttributeModifierValue& modifier) {
                           return modifier.attribute == attribute;
                         });
      item.attribute_modifiers.erase(end, item.attribute_modifiers.end());
      record_host_call("item_stack_remove_attribute_modifiers",
                       static_cast<std::int64_t>(item.attribute_modifiers.size()));
      return;
    }
    case HostId::ItemStackClearAttributeModifiers: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.clear-attribute-modifiers");
      item.attribute_modifiers.clear();
      record_host_call("item_stack_clear_attribute_modifiers", 0);
      return;
    }
    case HostId::ItemStackSetCustomData: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-custom-data");
      const std::string namespaced = host_string(arguments[1], arguments[2]);
      const std::string key = host_string(arguments[3], arguments[4]);
      item.custom_data[namespaced][key] = read_nbt_tree(arguments[5], arguments[6], arguments[7]);
      record_host_call("item_stack_set_custom_data", 1);
      return;
    }
    case HostId::ItemStackGetCustomData: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-custom-data");
      const std::string namespaced = host_string(arguments[1], arguments[2]);
      const std::string key = host_string(arguments[3], arguments[4]);
      const auto namespace_it = item.custom_data.find(namespaced);
      const nbt::Value* value = nullptr;
      if (namespace_it != item.custom_data.end()) {
        const auto found = namespace_it->second.find(key);
        if (found != namespace_it->second.end())
          value = &found->second;
      }
      write_option_nbt(import, arguments[5], value);
      record_host_call("item_stack_get_custom_data", value ? 1 : 0);
      return;
    }
    case HostId::ItemStackRemoveCustomData: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.remove-custom-data");
      const std::string namespaced = host_string(arguments[1], arguments[2]);
      const std::string key = host_string(arguments[3], arguments[4]);
      const auto namespace_it = item.custom_data.find(namespaced);
      if (namespace_it != item.custom_data.end()) {
        namespace_it->second.erase(key);
        if (namespace_it->second.empty())
          item.custom_data.erase(namespace_it);
      }
      record_host_call("item_stack_remove_custom_data", 1);
      return;
    }
    case HostId::ItemStackHasCustomData: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.has-custom-data");
      const std::string namespaced = host_string(arguments[1], arguments[2]);
      const std::string key = host_string(arguments[3], arguments[4]);
      const auto namespace_it = item.custom_data.find(namespaced);
      const bool found = namespace_it != item.custom_data.end() &&
                         namespace_it->second.find(key) != namespace_it->second.end();
      record_host_call("item_stack_has_custom_data", found ? 1 : 0);
      push_i32(found ? 1u : 0u);
      return;
    }
    case HostId::ItemStackGetComponents: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-components");
      write_component_list(import, arguments[1], item.components);
      record_host_call("item_stack_get_components",
                       static_cast<std::int64_t>(item.components.size()));
      return;
    }
    case HostId::ItemStackSetComponent: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-component");
      const std::uint32_t pointer = arguments[2];
      const std::uint32_t length = arguments[3];
      std::vector<std::uint8_t> bytes;
      if (length != 0) {
        const std::uint8_t* source = memory_at(pointer, length);
        bytes.assign(source, source + length);
        charge_host_bytes(length);
      }
      item.components[arguments[1]] = std::move(bytes);
      record_host_call("item_stack_set_component", length);
      return;
    }
    case HostId::ItemStackRemoveComponent: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.remove-component");
      item.components.erase(arguments[1]);
      record_host_call("item_stack_remove_component", arguments[1]);
      return;
    }
    case HostId::ItemStackGetEnchantments: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-enchantments");
      write_enchantment_list(import, arguments[1], item.enchantments);
      record_host_call("item_stack_get_enchantments",
                       static_cast<std::int64_t>(item.enchantments.size()));
      return;
    }
    case HostId::ItemStackAddEnchantment: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.add-enchantment");
      const std::uint32_t enchantment = arguments[1];
      const std::uint32_t level = arguments[2];
      if (level == 0)
        item.enchantments.erase(enchantment);
      else
        item.enchantments[enchantment] = level;
      record_host_call("item_stack_add_enchantment", level);
      return;
    }
    case HostId::ItemStackRemoveEnchantment: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.remove-enchantment");
      item.enchantments.erase(arguments[1]);
      record_host_call("item_stack_remove_enchantment", arguments[1]);
      return;
    }
    case HostId::ItemStackGetCustomEnchantments: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-custom-enchantments");
      write_custom_enchantment_list(import, arguments[1], item.custom_enchantments);
      record_host_call("item_stack_get_custom_enchantments",
                       static_cast<std::int64_t>(item.custom_enchantments.size()));
      return;
    }
    case HostId::ItemStackAddCustomEnchantment: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.add-custom-enchantment");
      const std::string id = host_string(arguments[1], arguments[2]);
      item.custom_enchantments[id] = arguments[3];
      record_host_call("item_stack_add_custom_enchantment", arguments[3]);
      return;
    }
    case HostId::ItemStackRemoveCustomEnchantment: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.remove-custom-enchantment");
      const std::string id = host_string(arguments[1], arguments[2]);
      item.custom_enchantments.erase(id);
      record_host_call("item_stack_remove_custom_enchantment",
                       static_cast<std::int64_t>(id.size()));
      return;
    }
    case HostId::ItemStackGetCustomEnchantmentLevel: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-custom-enchantment-level");
      const std::string id = host_string(arguments[1], arguments[2]);
      const auto found = item.custom_enchantments.find(id);
      write_option_u32(arguments[3], found == item.custom_enchantments.end()
                                         ? std::nullopt
                                         : std::optional<std::uint32_t>(found->second));
      record_host_call(
          "item_stack_get_custom_enchantment_level",
          found == item.custom_enchantments.end() ? -1 : static_cast<std::int64_t>(found->second));
      return;
    }
    case HostId::ItemStackHasCustomEnchantment: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.has-custom-enchantment");
      const std::string id = host_string(arguments[1], arguments[2]);
      const bool found = item.custom_enchantments.find(id) != item.custom_enchantments.end();
      record_host_call("item_stack_has_custom_enchantment", found ? 1 : 0);
      push_i32(found ? 1u : 0u);
      return;
    }
    case HostId::ItemStackGetLore: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-lore");
      std::vector<std::uint32_t> handles;
      handles.reserve(item.lore.size());
      for (const TextComponent& line : item.lore)
        handles.push_back(new_text(line));
      write_handle_list(import, arguments[1], handles);
      record_host_call("item_stack_get_lore", static_cast<std::int64_t>(handles.size()));
      return;
    }
    case HostId::ItemStackSetLore: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-lore");
      const std::uint32_t pointer = arguments[1];
      const std::uint32_t count = arguments[2];
      std::vector<TextComponent> lore;
      std::vector<std::uint32_t> handles;
      lore.reserve(count);
      handles.reserve(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t handle = read_u32(pointer + i * 4);
        lore.push_back(text_at(handle, "item-stack.set-lore"));
        handles.push_back(handle);
      }
      for (std::uint32_t handle : handles)
        drop_resource(handle, ResourceKind::Text);
      item.lore = std::move(lore);
      record_host_call("item_stack_set_lore", static_cast<std::int64_t>(count));
      return;
    }
    case HostId::ItemStackAddLore: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.add-lore");
      item.lore.push_back(text_at(arguments[1], "item-stack.add-lore"));
      drop_resource(arguments[1], ResourceKind::Text);
      record_host_call("item_stack_add_lore", static_cast<std::int64_t>(item.lore.size()));
      return;
    }
    case HostId::ItemStackGetCustomName: {
      const ItemStackValue& item = item_at(arguments[0], "item-stack.get-custom-name");
      if (item.custom_name) {
        const std::uint32_t handle = new_text(*item.custom_name);
        write_option_handle(arguments[1], handle);
      } else {
        write_option_handle(arguments[1], std::nullopt);
      }
      record_host_call("item_stack_get_custom_name", item.custom_name ? 1 : 0);
      return;
    }
    case HostId::ItemStackSetCustomName: {
      ItemStackValue& item = item_at(arguments[0], "item-stack.set-custom-name");
      if (arguments[1] != 0) {
        item.custom_name = text_at(arguments[2], "item-stack.set-custom-name");
        drop_resource(arguments[2], ResourceKind::Text);
      } else {
        item.custom_name.reset();
      }
      record_host_call("item_stack_set_custom_name", arguments[1] != 0 ? 1 : 0);
      return;
    }
    case HostId::WaitableSetNew: {
      push_i32(next_waitable_set_++);
      return;
    }
    case HostId::WaitableSetJoin: {
      if (count != 2 || async_tasks_.find(arguments[0]) == async_tasks_.end())
        trap("waitable-set.join used an invalid subtask");
      if (arguments[1] != 0)
        waitable_sets_[arguments[1]].push_back(arguments[0]);
      return;
    }
    case HostId::WaitableSetWait: {
      auto set = waitable_sets_.find(arguments[0]);
      if (set == waitable_sets_.end())
        trap("waitable-set.wait used an invalid waitable set");
      for (auto it = set->second.begin(); it != set->second.end(); ++it) {
        const auto task = async_tasks_.find(*it);
        if (task == async_tasks_.end() || !task->second)
          continue;
        write_u32(arguments[1], *it);
        write_u32(arguments[1] + 4, 2); // Subtask.State.RETURNED
        set->second.erase(it);
        record_host_call("waitable_set_wait", 1);
        push_i32(1); // EventCode.SUBTASK
        return;
      }
      trap("waitable-set.wait has no completed subtask");
    }
    case HostId::TaskReturn: {
      if (task_.called)
        trap("task.return was called twice by one async export");
      if (arguments[0] != 0 && arguments[0] != 1)
        trap("task.return carries an invalid result discriminant");
      task_.called = true;
      task_.ok = arguments[0] == 0;
      if (!task_.ok)
        task_.error = host_string(arguments[1], arguments[2]);
      return;
    }
    case HostId::TaskReturnVoid:
      if (task_.called)
        trap("task.return was called twice by one async export");
      task_.called = true;
      task_.ok = true;
      return;
    case HostId::ResourceDrop:
      // A generic drop may still appear in a hand-built component; dropping
      // any live resource is the same lifetime operation.
      if (count == 1)
        resources_.erase(arguments[0]);
      return;
    case HostId::ResourceDropContext:
      drop_resource(arguments[0], ResourceKind::Context);
      return;
    case HostId::ResourceDropPlayer:
      drop_resource(arguments[0], ResourceKind::Player);
      return;
    case HostId::ResourceDropServer:
      drop_resource(arguments[0], ResourceKind::Server);
      return;
    case HostId::ResourceDropText:
      drop_resource(arguments[0], ResourceKind::Text);
      return;
    case HostId::ResourceDropItem:
      drop_resource(arguments[0], ResourceKind::Item);
      return;
    case HostId::ResourceDropCommand:
      drop_resource(arguments[0], ResourceKind::Command);
      return;
    case HostId::ResourceDropCommandNode:
      drop_resource(arguments[0], ResourceKind::CommandNode);
      return;
    case HostId::ResourceDropWorld:
      drop_resource(arguments[0], ResourceKind::World);
      return;
    case HostId::Stub:
      // Handled before the argument copy; a stub never needs the values.
      return;
    }
  }

  // Reads a load/store's two immediates.
  std::uint32_t memarg(const std::uint8_t* body, std::size_t size, std::size_t* pc) {
    std::uint32_t align = 0;
    std::uint32_t offset = 0;
    if (!body_uleb(body, size, pc, &align) || !body_uleb(body, size, pc, &offset))
      trap("a memory immediate runs past the body");
    return offset;
  }

  void execute(std::size_t stop_depth = 0);

  const ParsedModule& module_;
  Limits limits_;
  Host& host_;
  RunReport* report_;
  std::vector<std::uint8_t> memory_;
  std::vector<GlobalDef> globals_;
  std::vector<Table> tables_;
  std::vector<std::uint8_t> data_dropped_;
  std::vector<std::uint8_t> element_dropped_;
  std::map<std::uint32_t, ResourceKind> resources_;
  std::map<std::uint32_t, std::string> player_names_;
  std::map<std::uint32_t, std::string> world_names_;
  std::map<std::uint32_t, TextComponent> text_values_;
  std::map<std::uint32_t, ItemStackValue> item_values_;
  std::map<std::uint32_t, CommandValue> command_values_;
  std::map<std::uint32_t, CommandNodeValue> command_node_values_;
  std::uint32_t next_resource_ = 1;
  // handler-id by event-type, in registration order-independent form. The
  // tick dispatcher only needs to know whether a tick handler exists.
  std::map<std::uint32_t, std::uint32_t> event_handlers_;
  std::map<std::uint32_t, ScheduledTask> scheduled_tasks_;
  std::uint32_t next_scheduled_task_ = 1;
  std::map<std::int32_t, std::map<std::string, std::string>> translations_;
  std::uint64_t current_tick_ = 0;
  bool task_callback_enabled_ = false;
  std::uint32_t task_callback_ = 0;
  bool task_callback_status_ = false;
  bool task_callback_async_ = false;
  std::vector<Value> stack_;
  std::vector<Frame> frames_;
  std::chrono::steady_clock::time_point deadline_ = std::chrono::steady_clock::now();
  std::uint64_t steps_ = 0;
  std::uint64_t host_bytes_ = 0;
  std::uint32_t max_pages_ = 0;
};

const BlockEnds* find_block(const std::vector<BlockEnds>& blocks, std::size_t start) {
  const auto it = std::lower_bound(
      blocks.begin(), blocks.end(), start,
      [](const BlockEnds& block, std::size_t value) { return block.start < value; });
  if (it != blocks.end() && it->start == start)
    return &*it;
  return nullptr;
}

// Float helpers with the MVP's exact edge cases.
float wasm_min_f32(float a, float b) {
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<float>::quiet_NaN();
  if (a == 0.0f && b == 0.0f)
    return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
float wasm_max_f32(float a, float b) {
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<float>::quiet_NaN();
  if (a == 0.0f && b == 0.0f)
    return std::signbit(a) ? b : a;
  return a > b ? a : b;
}
double wasm_min_f64(double a, double b) {
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<double>::quiet_NaN();
  if (a == 0.0 && b == 0.0)
    return std::signbit(a) ? a : b;
  return a < b ? a : b;
}
double wasm_max_f64(double a, double b) {
  if (std::isnan(a) || std::isnan(b))
    return std::numeric_limits<double>::quiet_NaN();
  if (a == 0.0 && b == 0.0)
    return std::signbit(a) ? b : a;
  return a > b ? a : b;
}

std::uint32_t clz32(std::uint32_t value) {
  if (value == 0)
    return 32;
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint32_t>(__builtin_clz(value));
#else
  std::uint32_t count = 0;
  while ((value & 0x80000000u) == 0) {
    value <<= 1;
    ++count;
  }
  return count;
#endif
}

std::uint32_t ctz32(std::uint32_t value) {
  if (value == 0)
    return 32;
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint32_t>(__builtin_ctz(value));
#else
  std::uint32_t count = 0;
  while ((value & 1u) == 0) {
    value >>= 1;
    ++count;
  }
  return count;
#endif
}

std::uint32_t popcnt32(std::uint32_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint32_t>(__builtin_popcount(value));
#else
  std::uint32_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
#endif
}

std::uint64_t clz64(std::uint64_t value) {
  if (value == 0)
    return 64;
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint64_t>(__builtin_clzll(value));
#else
  std::uint64_t count = 0;
  while ((value & 0x8000000000000000ull) == 0) {
    value <<= 1;
    ++count;
  }
  return count;
#endif
}

std::uint64_t ctz64(std::uint64_t value) {
  if (value == 0)
    return 64;
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint64_t>(__builtin_ctzll(value));
#else
  std::uint64_t count = 0;
  while ((value & 1ull) == 0) {
    value >>= 1;
    ++count;
  }
  return count;
#endif
}

std::uint64_t popcnt64(std::uint64_t value) {
#if defined(__GNUC__) || defined(__clang__)
  return static_cast<std::uint64_t>(__builtin_popcountll(value));
#else
  std::uint64_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
#endif
}

std::uint32_t rotl32(std::uint32_t value, std::uint32_t amount) {
  amount &= 31;
  return amount == 0 ? value : (value << amount) | (value >> (32 - amount));
}
std::uint32_t rotr32(std::uint32_t value, std::uint32_t amount) {
  amount &= 31;
  return amount == 0 ? value : (value >> amount) | (value << (32 - amount));
}
std::uint64_t rotl64(std::uint64_t value, std::uint64_t amount) {
  amount &= 63;
  return amount == 0 ? value : (value << amount) | (value >> (64 - amount));
}
std::uint64_t rotr64(std::uint64_t value, std::uint64_t amount) {
  amount &= 63;
  return amount == 0 ? value : (value >> amount) | (value << (64 - amount));
}

std::int32_t trunc_i32(double value) {
  if (std::isnan(value))
    trap("invalid conversion to integer");
  const double truncated = std::trunc(value);
  if (!(truncated >= -2147483648.0 && truncated < 2147483648.0))
    trap("integer overflow in conversion");
  return static_cast<std::int32_t>(truncated);
}
std::uint32_t trunc_u32(double value) {
  if (std::isnan(value))
    trap("invalid conversion to integer");
  const double truncated = std::trunc(value);
  if (!(truncated > -1.0 && truncated < 4294967296.0))
    trap("integer overflow in conversion");
  return static_cast<std::uint32_t>(truncated);
}
std::int64_t trunc_i64(double value) {
  if (std::isnan(value))
    trap("invalid conversion to integer");
  const double truncated = std::trunc(value);
  if (!(truncated >= -9223372036854775808.0 && truncated < 9223372036854775808.0))
    trap("integer overflow in conversion");
  return static_cast<std::int64_t>(truncated);
}
std::uint64_t trunc_u64(double value) {
  if (std::isnan(value))
    trap("invalid conversion to integer");
  const double truncated = std::trunc(value);
  if (!(truncated > -1.0 && truncated < 18446744073709551616.0))
    trap("integer overflow in conversion");
  return static_cast<std::uint64_t>(truncated);
}

// WebAssembly's saturating conversions (0xfc 0..7) have no trap path. NaN
// becomes zero and every out-of-range value clamps to the destination's bound.
std::int32_t trunc_sat_i32(double value) {
  if (std::isnan(value))
    return 0;
  if (value <= -2147483648.0)
    return std::numeric_limits<std::int32_t>::min();
  if (value >= 2147483648.0)
    return std::numeric_limits<std::int32_t>::max();
  return static_cast<std::int32_t>(std::trunc(value));
}
std::uint32_t trunc_sat_u32(double value) {
  if (std::isnan(value) || value <= 0.0)
    return 0;
  if (value >= 4294967296.0)
    return std::numeric_limits<std::uint32_t>::max();
  return static_cast<std::uint32_t>(std::trunc(value));
}
std::int64_t trunc_sat_i64(double value) {
  if (std::isnan(value))
    return 0;
  if (value <= -9223372036854775808.0)
    return std::numeric_limits<std::int64_t>::min();
  if (value >= 9223372036854775808.0)
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(std::trunc(value));
}
std::uint64_t trunc_sat_u64(double value) {
  if (std::isnan(value) || value <= 0.0)
    return 0;
  if (value >= 18446744073709551616.0)
    return std::numeric_limits<std::uint64_t>::max();
  return static_cast<std::uint64_t>(std::trunc(value));
}

void Machine::execute(std::size_t stop_depth) {
  while (frames_.size() > stop_depth) {
    Frame& frame = frames_.back();
    const std::uint8_t* body = frame.body;
    const std::size_t size = frame.size;
    std::size_t pc = frame.pc;
    if (pc >= size) {
      do_return();
      continue;
    }
    const std::size_t at = pc;
    const std::uint8_t opcode = body[pc++];
    ++steps_;
    if (steps_ > limits_.max_steps)
      budget("step budget exhausted (" + std::to_string(limits_.max_steps) + " instructions)");
    if ((steps_ & 1023) == 0)
      check_clock();

    switch (opcode) {
    case 0x00: // unreachable
      trap("unreachable executed");
    case 0x01: // nop
      break;
    case 0x02: { // block
      const BlockEnds* block = find_block(frame.function->blocks, at);
      if (block == nullptr)
        trap("a block has no matching end");
      // The body starts after the block-type byte, which the loader consumed.
      pc = block->body;
      frame.labels.push_back(Label{false, stack_.size(), block->arity, block->arity, block->end});
      break;
    }
    case 0x03: { // loop
      const BlockEnds* block = find_block(frame.function->blocks, at);
      if (block == nullptr)
        trap("a loop has no matching end");
      pc = block->body;
      frame.labels.push_back(Label{true, stack_.size(), 0, block->arity, block->body});
      break;
    }
    case 0x04: { // if
      const BlockEnds* block = find_block(frame.function->blocks, at);
      if (block == nullptr)
        trap("an `if` has no matching end");
      const std::uint32_t condition = as_u32(pop());
      if (condition != 0) {
        pc = block->body;
        frame.labels.push_back(Label{false, stack_.size(), block->arity, block->arity, block->end});
      } else if (block->has_else) {
        frame.labels.push_back(Label{false, stack_.size(), block->arity, block->arity, block->end});
        pc = block->else_body;
      } else {
        unwind_to(stack_.size(), 0);
        pc = block->end;
      }
      break;
    }
    case 0x05: { // else: the then-branch is done
      if (frame.labels.empty())
        trap("`else` without a label");
      const Label label = frame.labels.back();
      frame.labels.pop_back();
      unwind_to(label.height, label.end_arity);
      pc = label.target;
      break;
    }
    case 0x0b: { // end
      if (frame.labels.empty()) {
        frames_.back().pc = pc;
        do_return();
        continue;
      }
      const Label label = frame.labels.back();
      frame.labels.pop_back();
      unwind_to(label.height, label.end_arity);
      break;
    }
    case 0x0c: { // br
      std::uint32_t depth = 0;
      if (!body_uleb(body, size, &pc, &depth))
        trap("a branch immediate runs past the body");
      if (depth >= frame.labels.size()) {
        frames_.back().pc = pc;
        do_return();
        continue;
      }
      const Label& label = frame.labels[frame.labels.size() - 1 - depth];
      // `br 0` back to the innermost loop is the interpreter's hottest branch.
      // A loop label stays on the label stack and a loop takes no branch
      // arguments, so skip the generic copy-and-resize path.
      if (label.is_loop && depth == 0) {
        unwind_to(label.height, 0);
        pc = label.target;
        break;
      }
      const Label target = label;
      unwind_to(target.height, target.branch_arity);
      frame.labels.resize(frame.labels.size() - (target.is_loop ? depth : depth + 1));
      pc = target.target;
      break;
    }
    case 0x0d: { // br_if
      std::uint32_t depth = 0;
      if (!body_uleb(body, size, &pc, &depth))
        trap("a branch immediate runs past the body");
      if (as_u32(pop()) == 0)
        break;
      if (depth >= frame.labels.size()) {
        frames_.back().pc = pc;
        do_return();
        continue;
      }
      const Label& label = frame.labels[frame.labels.size() - 1 - depth];
      if (label.is_loop && depth == 0) {
        unwind_to(label.height, 0);
        pc = label.target;
        break;
      }
      const Label target = label;
      unwind_to(target.height, target.branch_arity);
      frame.labels.resize(frame.labels.size() - (target.is_loop ? depth : depth + 1));
      pc = target.target;
      break;
    }
    case 0x0e: { // br_table
      std::uint32_t count = 0;
      if (!body_uleb(body, size, &pc, &count))
        trap("br_table's target count runs past the body");
      // Every target is one byte or more, so a count larger than the body is a
      // malformed module rather than something to allocate for.
      if (count > size)
        trap("br_table's target count runs past the body");
      const std::size_t table_start = pc;
      std::uint32_t fallback = 0;
      for (std::uint32_t i = 0; i <= count; ++i) {
        std::uint32_t depth = 0;
        if (!body_uleb(body, size, &pc, &depth))
          trap("a br_table target runs past the body");
        if (i == count)
          fallback = depth;
      }
      const std::uint32_t index = as_u32(pop());
      std::uint32_t selected = fallback;
      if (index < count) {
        std::size_t cursor = table_start;
        for (std::uint32_t i = 0; i <= index; ++i) {
          if (!body_uleb(body, size, &cursor, &selected))
            trap("a br_table target runs past the body");
        }
      }
      if (selected >= frame.labels.size()) {
        frames_.back().pc = pc;
        do_return();
        continue;
      }
      const Label label = frame.labels[frame.labels.size() - 1 - selected];
      unwind_to(label.height, label.branch_arity);
      frame.labels.resize(frame.labels.size() - (label.is_loop ? selected : selected + 1));
      pc = label.target;
      break;
    }
    case 0x0f: // return
      frames_.back().pc = pc;
      do_return();
      continue;
    case 0x10: { // call
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index))
        trap("a call immediate runs past the body");
      frames_.back().pc = pc;
      call_index(index);
      continue;
    }
    case 0x11: { // call_indirect
      std::uint32_t type_index = 0;
      std::uint32_t table_index = 0;
      if (!body_uleb(body, size, &pc, &type_index) || !body_uleb(body, size, &pc, &table_index))
        trap("a call_indirect immediate runs past the body");
      if (type_index >= module_.types.size())
        trap("call_indirect names a type that does not exist");
      const Table& table = table_at(table_index);
      const std::uint32_t table_entry = as_u32(pop());
      if (table_entry >= table.entries.size())
        trap("call_indirect table index " + std::to_string(table_entry) + " is outside the table");
      const std::uint32_t ref = table.entries[table_entry];
      if (ref == 0)
        trap("indirect call through a null table entry");
      const std::uint32_t function = ref - 1;
      if (function >= module_.function_count())
        trap("indirect call names a function that does not exist");
      const FuncType* actual = function_type(function);
      if (actual == nullptr || !same_func_type(*actual, module_.types[type_index]))
        trap("indirect call type mismatch: table entry " + std::to_string(ref) +
             " points to function " + std::to_string(function) + " declared " +
             (actual == nullptr ? std::string("without a type") : signature_text(*actual)) +
             ", expected " + signature_text(module_.types[type_index]));
      frame.pc = pc;
      call_index(function);
      continue;
    }
    case 0x1a: // drop
      pop();
      break;
    case 0x1b: { // select
      const std::uint32_t condition = as_u32(pop());
      const Value other = pop();
      const Value first = pop();
      push(condition != 0 ? first : other);
      break;
    }
    case 0x1c: { // typed select
      std::uint32_t count = 0;
      if (!body_uleb(body, size, &pc, &count) || count != 1)
        trap("a typed select immediate is malformed");
      if (pc >= size)
        trap("a typed select result type runs past the body");
      ++pc;
      const std::uint32_t condition = as_u32(pop());
      const Value other = pop();
      const Value first = pop();
      push(condition != 0 ? first : other);
      break;
    }
    case 0x20: { // local.get
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= frame.locals.size())
        trap("local.get reads a local that does not exist");
      push(frame.locals[index]);
      break;
    }
    case 0x21: { // local.set
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= frame.locals.size())
        trap("local.set writes a local that does not exist");
      frame.locals[index] = pop();
      break;
    }
    case 0x22: { // local.tee
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= frame.locals.size())
        trap("local.tee writes a local that does not exist");
      if (stack_.empty())
        trap("operand stack underflow");
      frame.locals[index] = stack_.back();
      break;
    }
    case 0x23: { // global.get
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= globals_.size())
        trap("global.get reads a global that does not exist");
      push(globals_[index].value);
      break;
    }
    case 0x24: { // global.set
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= globals_.size())
        trap("global.set writes a global that does not exist");
      if (!globals_[index].mut)
        trap("global.set writes an immutable global");
      globals_[index].value = pop();
      break;
    }
    case 0x25: { // table.get
      std::uint32_t table_index = 0;
      if (!body_uleb(body, size, &pc, &table_index))
        trap("table.get runs past the body");
      const Table& table = table_at(table_index);
      const std::uint32_t index = as_u32(pop());
      if (index >= table.entries.size())
        trap("table.get is outside the table");
      push(Value{ValType::FuncRef, table.entries[index]});
      break;
    }
    case 0x26: { // table.set
      std::uint32_t table_index = 0;
      if (!body_uleb(body, size, &pc, &table_index))
        trap("table.set runs past the body");
      Table& table = table_at(table_index);
      const Value value = pop();
      if (value.type != ValType::FuncRef)
        trap("table.set value is not a funcref");
      const std::uint32_t index = as_u32(pop());
      if (index >= table.entries.size())
        trap("table.set is outside the table");
      table.entries[index] = as_u32(value);
      break;
    }
    case 0x28: { // i32.load
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint32_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 4), 4);
      push_i32(value);
      break;
    }
    case 0x29: { // i64.load
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint64_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 8), 8);
      push(make_i64(value));
      break;
    }
    case 0x2a: { // f32.load
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint32_t bits = 0;
      std::memcpy(&bits, memory_at(as_u32(pop()) + offset, 4), 4);
      push(Value{ValType::F32, bits});
      break;
    }
    case 0x2b: { // f64.load
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint64_t bits = 0;
      std::memcpy(&bits, memory_at(as_u32(pop()) + offset, 8), 8);
      push(Value{ValType::F64, bits});
      break;
    }
    case 0x2c: { // i32.load8_s
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint8_t raw = *memory_at(as_u32(pop()) + offset, 1);
      push_i32(
          static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(raw))));
      break;
    }
    case 0x2d: { // i32.load8_u
      const std::uint32_t offset = memarg(body, size, &pc);
      push_i32(*memory_at(as_u32(pop()) + offset, 1));
      break;
    }
    case 0x2e: { // i32.load16_s
      const std::uint32_t offset = memarg(body, size, &pc);
      std::int16_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 2), 2);
      push_i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
      break;
    }
    case 0x2f: { // i32.load16_u
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint16_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 2), 2);
      push_i32(value);
      break;
    }
    case 0x30: { // i64.load8_s
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint8_t raw = *memory_at(as_u32(pop()) + offset, 1);
      push(make_i64(
          static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)))));
      break;
    }
    case 0x31: { // i64.load8_u
      const std::uint32_t offset = memarg(body, size, &pc);
      push(make_i64(*memory_at(as_u32(pop()) + offset, 1)));
      break;
    }
    case 0x32: { // i64.load16_s
      const std::uint32_t offset = memarg(body, size, &pc);
      std::int16_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 2), 2);
      push(make_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value))));
      break;
    }
    case 0x33: { // i64.load16_u
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint16_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 2), 2);
      push(make_i64(value));
      break;
    }
    case 0x34: { // i64.load32_s
      const std::uint32_t offset = memarg(body, size, &pc);
      std::int32_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 4), 4);
      push(make_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(value))));
      break;
    }
    case 0x35: { // i64.load32_u
      const std::uint32_t offset = memarg(body, size, &pc);
      std::uint32_t value = 0;
      std::memcpy(&value, memory_at(as_u32(pop()) + offset, 4), 4);
      push(make_i64(value));
      break;
    }
    case 0x36: { // i32.store
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint32_t value = as_u32(pop());
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 4), &value, 4);
      break;
    }
    case 0x37: { // i64.store
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint64_t value = as_u64(pop());
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 8), &value, 8);
      break;
    }
    case 0x38: { // f32.store
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint32_t value = as_u32(pop());
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 4), &value, 4);
      break;
    }
    case 0x39: { // f64.store
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint64_t value = as_u64(pop());
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 8), &value, 8);
      break;
    }
    case 0x3a: { // i32.store8
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint8_t value = static_cast<std::uint8_t>(as_u32(pop()));
      const std::uint32_t address = as_u32(pop()) + offset;
      *memory_at(address, 1) = value;
      break;
    }
    case 0x3b: { // i32.store16
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint16_t value = static_cast<std::uint16_t>(as_u32(pop()));
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 2), &value, 2);
      break;
    }
    case 0x3c: { // i64.store8
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint8_t value = static_cast<std::uint8_t>(as_u64(pop()));
      const std::uint32_t address = as_u32(pop()) + offset;
      *memory_at(address, 1) = value;
      break;
    }
    case 0x3d: { // i64.store16
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint16_t value = static_cast<std::uint16_t>(as_u64(pop()));
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 2), &value, 2);
      break;
    }
    case 0x3e: { // i64.store32
      const std::uint32_t offset = memarg(body, size, &pc);
      const std::uint32_t value = static_cast<std::uint32_t>(as_u64(pop()));
      const std::uint32_t address = as_u32(pop()) + offset;
      std::memcpy(memory_at(address, 4), &value, 4);
      break;
    }
    case 0x3f: { // memory.size
      if (pc >= size)
        trap("memory.size runs past the body");
      ++pc;
      push_i32(static_cast<std::uint32_t>(memory_.size() / kPageBytes));
      break;
    }
    case 0x40: { // memory.grow
      if (pc >= size)
        trap("memory.grow runs past the body");
      ++pc;
      const std::uint32_t delta = as_u32(pop());
      const std::uint64_t current = memory_.size() / kPageBytes;
      if (delta > max_pages_ || current + delta > max_pages_) {
        push_i32(0xffffffffu);
        break;
      }
      memory_.resize((current + delta) * kPageBytes, 0);
      if (memory_.size() / kPageBytes > report_->peak_pages)
        report_->peak_pages = static_cast<std::uint32_t>(memory_.size() / kPageBytes);
      push_i32(static_cast<std::uint32_t>(current));
      break;
    }
    case 0x41: { // i32.const
      std::uint64_t raw = 0;
      if (!body_sleb(body, size, &pc, &raw))
        trap("an i32 constant runs past the body");
      push_i32(static_cast<std::uint32_t>(raw));
      break;
    }
    case 0x42: { // i64.const
      std::uint64_t raw = 0;
      if (!body_sleb(body, size, &pc, &raw))
        trap("an i64 constant runs past the body");
      push(make_i64(raw));
      break;
    }
    case 0x43: { // f32.const
      if (size - pc < 4)
        trap("an f32 constant runs past the body");
      std::uint32_t bits = 0;
      std::memcpy(&bits, body + pc, 4);
      pc += 4;
      push(Value{ValType::F32, bits});
      break;
    }
    case 0x44: { // f64.const
      if (size - pc < 8)
        trap("an f64 constant runs past the body");
      std::uint64_t bits = 0;
      std::memcpy(&bits, body + pc, 8);
      pc += 8;
      push(Value{ValType::F64, bits});
      break;
    }
    case 0x45: { // i32.eqz
      push_i32(as_u32(pop()) == 0 ? 1 : 0);
      break;
    }
    case 0x46: { // i32.eq
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a == b ? 1 : 0);
      break;
    }
    case 0x47: { // i32.ne
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a != b ? 1 : 0);
      break;
    }
    case 0x48: { // i32.lt_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x49: { // i32.lt_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x4a: { // i32.gt_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x4b: { // i32.gt_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x4c: { // i32.le_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x4d: { // i32.le_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x4e: { // i32.ge_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x4f: { // i32.ge_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x50: { // i64.eqz
      push_i32(as_u64(pop()) == 0 ? 1 : 0);
      break;
    }
    case 0x51: { // i64.eq
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a == b ? 1 : 0);
      break;
    }
    case 0x52: { // i64.ne
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a != b ? 1 : 0);
      break;
    }
    case 0x53: { // i64.lt_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x54: { // i64.lt_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x55: { // i64.gt_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x56: { // i64.gt_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x57: { // i64.le_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x58: { // i64.le_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x59: { // i64.ge_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x5a: { // i64.ge_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x5b: { // f32.eq
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a == b ? 1 : 0);
      break;
    }
    case 0x5c: { // f32.ne
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a != b ? 1 : 0);
      break;
    }
    case 0x5d: { // f32.lt
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x5e: { // f32.gt
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x5f: { // f32.le
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x60: { // f32.ge
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x61: { // f64.eq
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a == b ? 1 : 0);
      break;
    }
    case 0x62: { // f64.ne
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a != b ? 1 : 0);
      break;
    }
    case 0x63: { // f64.lt
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a < b ? 1 : 0);
      break;
    }
    case 0x64: { // f64.gt
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a > b ? 1 : 0);
      break;
    }
    case 0x65: { // f64.le
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a <= b ? 1 : 0);
      break;
    }
    case 0x66: { // f64.ge
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push_i32(a >= b ? 1 : 0);
      break;
    }
    case 0x67: // i32.clz
      push_i32(clz32(as_u32(pop())));
      break;
    case 0x68: // i32.ctz
      push_i32(ctz32(as_u32(pop())));
      break;
    case 0x69: // i32.popcnt
      push_i32(popcnt32(as_u32(pop())));
      break;
    case 0x6a: { // i32.add
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a + b);
      break;
    }
    case 0x6b: { // i32.sub
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a - b);
      break;
    }
    case 0x6c: { // i32.mul
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a * b);
      break;
    }
    case 0x6d: { // i32.div_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      if (b == 0)
        trap("integer divide by zero");
      if (a == std::numeric_limits<std::int32_t>::min() && b == -1)
        trap("integer overflow");
      push_i32(static_cast<std::uint32_t>(a / b));
      break;
    }
    case 0x6e: { // i32.div_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      if (b == 0)
        trap("integer divide by zero");
      push_i32(a / b);
      break;
    }
    case 0x6f: { // i32.rem_s
      const std::int32_t b = as_i32(pop());
      const std::int32_t a = as_i32(pop());
      if (b == 0)
        trap("integer divide by zero");
      push_i32(a == std::numeric_limits<std::int32_t>::min() && b == -1
                   ? 0u
                   : static_cast<std::uint32_t>(a % b));
      break;
    }
    case 0x70: { // i32.rem_u
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      if (b == 0)
        trap("integer divide by zero");
      push_i32(a % b);
      break;
    }
    case 0x71: { // i32.and
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a & b);
      break;
    }
    case 0x72: { // i32.or
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a | b);
      break;
    }
    case 0x73: { // i32.xor
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(a ^ b);
      break;
    }
    case 0x74: { // i32.shl
      const std::uint32_t b = as_u32(pop()) & 31u;
      const std::uint32_t a = as_u32(pop());
      push_i32(a << b);
      break;
    }
    case 0x75: { // i32.shr_s
      const std::uint32_t b = as_u32(pop()) & 31u;
      const std::int32_t a = as_i32(pop());
      push_i32(static_cast<std::uint32_t>(a >> b));
      break;
    }
    case 0x76: { // i32.shr_u
      const std::uint32_t b = as_u32(pop()) & 31u;
      const std::uint32_t a = as_u32(pop());
      push_i32(a >> b);
      break;
    }
    case 0x77: { // i32.rotl
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(rotl32(a, b));
      break;
    }
    case 0x78: { // i32.rotr
      const std::uint32_t b = as_u32(pop());
      const std::uint32_t a = as_u32(pop());
      push_i32(rotr32(a, b));
      break;
    }
    case 0x79: // i64.clz
      push(make_i64(clz64(as_u64(pop()))));
      break;
    case 0x7a: // i64.ctz
      push(make_i64(ctz64(as_u64(pop()))));
      break;
    case 0x7b: // i64.popcnt
      push(make_i64(popcnt64(as_u64(pop()))));
      break;
    case 0x7c: { // i64.add
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a + b));
      break;
    }
    case 0x7d: { // i64.sub
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a - b));
      break;
    }
    case 0x7e: { // i64.mul
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a * b));
      break;
    }
    case 0x7f: { // i64.div_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      if (b == 0)
        trap("integer divide by zero");
      if (a == std::numeric_limits<std::int64_t>::min() && b == -1)
        trap("integer overflow");
      push(make_i64(static_cast<std::uint64_t>(a / b)));
      break;
    }
    case 0x80: { // i64.div_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      if (b == 0)
        trap("integer divide by zero");
      push(make_i64(a / b));
      break;
    }
    case 0x81: { // i64.rem_s
      const std::int64_t b = as_i64(pop());
      const std::int64_t a = as_i64(pop());
      if (b == 0)
        trap("integer divide by zero");
      push(make_i64(a == std::numeric_limits<std::int64_t>::min() && b == -1
                        ? 0ull
                        : static_cast<std::uint64_t>(a % b)));
      break;
    }
    case 0x82: { // i64.rem_u
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      if (b == 0)
        trap("integer divide by zero");
      push(make_i64(a % b));
      break;
    }
    case 0x83: { // i64.and
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a & b));
      break;
    }
    case 0x84: { // i64.or
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a | b));
      break;
    }
    case 0x85: { // i64.xor
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a ^ b));
      break;
    }
    case 0x86: { // i64.shl
      const std::uint64_t b = as_u64(pop()) & 63u;
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a << b));
      break;
    }
    case 0x87: { // i64.shr_s
      const std::uint64_t b = as_u64(pop()) & 63u;
      const std::int64_t a = as_i64(pop());
      push(make_i64(static_cast<std::uint64_t>(a >> b)));
      break;
    }
    case 0x88: { // i64.shr_u
      const std::uint64_t b = as_u64(pop()) & 63u;
      const std::uint64_t a = as_u64(pop());
      push(make_i64(a >> b));
      break;
    }
    case 0x89: { // i64.rotl
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(rotl64(a, b)));
      break;
    }
    case 0x8a: { // i64.rotr
      const std::uint64_t b = as_u64(pop());
      const std::uint64_t a = as_u64(pop());
      push(make_i64(rotr64(a, b)));
      break;
    }
    case 0x8b: // f32.abs
      push(make_f32(std::fabs(as_f32(pop()))));
      break;
    case 0x8c: // f32.neg
      push(make_f32(-as_f32(pop())));
      break;
    case 0x8d: // f32.ceil
      push(make_f32(std::ceil(as_f32(pop()))));
      break;
    case 0x8e: // f32.floor
      push(make_f32(std::floor(as_f32(pop()))));
      break;
    case 0x8f: // f32.trunc
      push(make_f32(std::trunc(as_f32(pop()))));
      break;
    case 0x90: // f32.nearest
      push(make_f32(std::nearbyint(as_f32(pop()))));
      break;
    case 0x91: // f32.sqrt
      push(make_f32(std::sqrt(as_f32(pop()))));
      break;
    case 0x92: { // f32.add
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(a + b));
      break;
    }
    case 0x93: { // f32.sub
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(a - b));
      break;
    }
    case 0x94: { // f32.mul
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(a * b));
      break;
    }
    case 0x95: { // f32.div
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(a / b));
      break;
    }
    case 0x96: { // f32.min
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(wasm_min_f32(a, b)));
      break;
    }
    case 0x97: { // f32.max
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(wasm_max_f32(a, b)));
      break;
    }
    case 0x98: { // f32.copysign
      const float b = as_f32(pop());
      const float a = as_f32(pop());
      push(make_f32(std::copysign(a, b)));
      break;
    }
    case 0x99: // f64.abs
      push(make_f64(std::fabs(as_f64(pop()))));
      break;
    case 0x9a: // f64.neg
      push(make_f64(-as_f64(pop())));
      break;
    case 0x9b: // f64.ceil
      push(make_f64(std::ceil(as_f64(pop()))));
      break;
    case 0x9c: // f64.floor
      push(make_f64(std::floor(as_f64(pop()))));
      break;
    case 0x9d: // f64.trunc
      push(make_f64(std::trunc(as_f64(pop()))));
      break;
    case 0x9e: // f64.nearest
      push(make_f64(std::nearbyint(as_f64(pop()))));
      break;
    case 0x9f: // f64.sqrt
      push(make_f64(std::sqrt(as_f64(pop()))));
      break;
    case 0xa0: { // f64.add
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(a + b));
      break;
    }
    case 0xa1: { // f64.sub
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(a - b));
      break;
    }
    case 0xa2: { // f64.mul
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(a * b));
      break;
    }
    case 0xa3: { // f64.div
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(a / b));
      break;
    }
    case 0xa4: { // f64.min
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(wasm_min_f64(a, b)));
      break;
    }
    case 0xa5: { // f64.max
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(wasm_max_f64(a, b)));
      break;
    }
    case 0xa6: { // f64.copysign
      const double b = as_f64(pop());
      const double a = as_f64(pop());
      push(make_f64(std::copysign(a, b)));
      break;
    }
    case 0xa7: // i32.wrap_i64
      push_i32(static_cast<std::uint32_t>(as_u64(pop())));
      break;
    case 0xa8: // i32.trunc_f32_s
      push_i32(static_cast<std::uint32_t>(trunc_i32(as_f32(pop()))));
      break;
    case 0xa9: // i32.trunc_f32_u
      push_i32(trunc_u32(as_f32(pop())));
      break;
    case 0xaa: // i32.trunc_f64_s
      push_i32(static_cast<std::uint32_t>(trunc_i32(as_f64(pop()))));
      break;
    case 0xab: // i32.trunc_f64_u
      push_i32(trunc_u32(as_f64(pop())));
      break;
    case 0xac: // i64.extend_i32_s
      push(make_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(as_i32(pop())))));
      break;
    case 0xad: // i64.extend_i32_u
      push(make_i64(as_u32(pop())));
      break;
    case 0xae: // i64.trunc_f32_s
      push(make_i64(static_cast<std::uint64_t>(trunc_i64(as_f32(pop())))));
      break;
    case 0xaf: // i64.trunc_f32_u
      push(make_i64(trunc_u64(as_f32(pop()))));
      break;
    case 0xb0: // i64.trunc_f64_s
      push(make_i64(static_cast<std::uint64_t>(trunc_i64(as_f64(pop())))));
      break;
    case 0xb1: // i64.trunc_f64_u
      push(make_i64(trunc_u64(as_f64(pop()))));
      break;
    case 0xb2: // f32.convert_i32_s
      push(make_f32(static_cast<float>(as_i32(pop()))));
      break;
    case 0xb3: // f32.convert_i32_u
      push(make_f32(static_cast<float>(as_u32(pop()))));
      break;
    case 0xb4: // f32.convert_i64_s
      push(make_f32(static_cast<float>(as_i64(pop()))));
      break;
    case 0xb5: // f32.convert_i64_u
      push(make_f32(static_cast<float>(as_u64(pop()))));
      break;
    case 0xb6: // f32.demote_f64
      push(make_f32(static_cast<float>(as_f64(pop()))));
      break;
    case 0xb7: // f64.convert_i32_s
      push(make_f64(static_cast<double>(as_i32(pop()))));
      break;
    case 0xb8: // f64.convert_i32_u
      push(make_f64(static_cast<double>(as_u32(pop()))));
      break;
    case 0xb9: // f64.convert_i64_s
      push(make_f64(static_cast<double>(as_i64(pop()))));
      break;
    case 0xba: // f64.convert_i64_u
      push(make_f64(static_cast<double>(as_u64(pop()))));
      break;
    case 0xbb: // f64.promote_f32
      push(make_f64(static_cast<double>(as_f32(pop()))));
      break;
    case 0xbc: { // i32.reinterpret_f32
      const Value value = pop();
      push_i32(as_u32(value));
      break;
    }
    case 0xbd: { // i64.reinterpret_f64
      const Value value = pop();
      push(make_i64(value.bits));
      break;
    }
    case 0xbe: { // f32.reinterpret_i32
      const Value value = pop();
      push(Value{ValType::F32, as_u32(value)});
      break;
    }
    case 0xbf: { // f64.reinterpret_i64
      const Value value = pop();
      push(Value{ValType::F64, value.bits});
      break;
    }
    case 0xc0: // i32.extend8_s
      push_i32(static_cast<std::uint32_t>(static_cast<std::int32_t>(
          static_cast<std::int8_t>(static_cast<std::uint8_t>(as_u32(pop()))))));
      break;
    case 0xc1: // i32.extend16_s
      push_i32(static_cast<std::uint32_t>(
          static_cast<std::int32_t>(static_cast<std::int16_t>(as_u32(pop())))));
      break;
    case 0xc2: // i64.extend8_s
      push(make_i64(static_cast<std::uint64_t>(static_cast<std::int64_t>(
          static_cast<std::int8_t>(static_cast<std::uint8_t>(as_u64(pop())))))));
      break;
    case 0xc3: // i64.extend16_s
      push(make_i64(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(static_cast<std::int16_t>(as_u64(pop()))))));
      break;
    case 0xc4: // i64.extend32_s
      push(make_i64(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(static_cast<std::int32_t>(as_u64(pop()))))));
      break;
    case 0xd0: { // ref.null
      if (pc >= size)
        trap("ref.null runs past the body");
      const std::uint8_t type = body[pc++];
      if (type != 0x70)
        trap("ref.null is not funcref");
      push(Value{ValType::FuncRef, 0});
      break;
    }
    case 0xd1: { // ref.is_null
      const Value value = pop();
      if (value.type != ValType::FuncRef)
        trap("ref.is_null operand is not a funcref");
      push_i32(value.bits == 0 ? 1 : 0);
      break;
    }
    case 0xd2: { // ref.func
      std::uint32_t index = 0;
      if (!body_uleb(body, size, &pc, &index) || index >= module_.function_count())
        trap("ref.func names a function that does not exist");
      push(Value{ValType::FuncRef, static_cast<std::uint64_t>(index) + 1});
      break;
    }
    case 0xfc: { // saturating conversions and bulk memory
      std::uint32_t sub = 0;
      if (!body_uleb(body, size, &pc, &sub))
        trap("a 0xfc subopcode runs past the body");
      switch (sub) {
      case 0: // i32.trunc_sat_f32_s
        push_i32(static_cast<std::uint32_t>(trunc_sat_i32(as_f32(pop()))));
        break;
      case 1: // i32.trunc_sat_f32_u
        push_i32(trunc_sat_u32(as_f32(pop())));
        break;
      case 2: // i32.trunc_sat_f64_s
        push_i32(static_cast<std::uint32_t>(trunc_sat_i32(as_f64(pop()))));
        break;
      case 3: // i32.trunc_sat_f64_u
        push_i32(trunc_sat_u32(as_f64(pop())));
        break;
      case 4: // i64.trunc_sat_f32_s
        push(make_i64(static_cast<std::uint64_t>(trunc_sat_i64(as_f32(pop())))));
        break;
      case 5: // i64.trunc_sat_f32_u
        push(make_i64(trunc_sat_u64(as_f32(pop()))));
        break;
      case 6: // i64.trunc_sat_f64_s
        push(make_i64(static_cast<std::uint64_t>(trunc_sat_i64(as_f64(pop())))));
        break;
      case 7: // i64.trunc_sat_f64_u
        push(make_i64(trunc_sat_u64(as_f64(pop()))));
        break;
      case 8: { // memory.init dataidx memidx
        std::uint32_t data_index = 0;
        std::uint32_t memory_index = 0;
        if (!body_uleb(body, size, &pc, &data_index) ||
            !body_uleb(body, size, &pc, &memory_index) || memory_index != 0)
          trap("memory.init has a malformed immediate");
        const std::uint32_t count = as_u32(pop());
        const std::uint32_t source = as_u32(pop());
        const std::uint32_t destination = as_u32(pop());
        if (data_index >= module_.data.size())
          trap("memory.init names a data segment that does not exist");
        const DataSegment& segment = module_.data[data_index];
        if (count != 0 && data_dropped_[data_index])
          trap("memory.init names a dropped data segment");
        if (static_cast<std::uint64_t>(source) + count > segment.bytes.size())
          trap("memory.init source is outside the data segment");
        std::uint8_t* target = memory_at(destination, count);
        if (count != 0)
          std::memcpy(target, segment.bytes.data() + source, count);
        break;
      }
      case 9: { // data.drop
        std::uint32_t data_index = 0;
        if (!body_uleb(body, size, &pc, &data_index) || data_index >= module_.data.size())
          trap("data.drop names a data segment that does not exist");
        data_dropped_[data_index] = 1;
        break;
      }
      case 10: { // memory.copy dstmem srcmem
        std::uint32_t destination_memory = 0;
        std::uint32_t source_memory = 0;
        if (!body_uleb(body, size, &pc, &destination_memory) ||
            !body_uleb(body, size, &pc, &source_memory) || destination_memory != 0 ||
            source_memory != 0)
          trap("memory.copy has a malformed immediate");
        const std::uint32_t count = as_u32(pop());
        const std::uint32_t source = as_u32(pop());
        const std::uint32_t destination = as_u32(pop());
        std::uint8_t* source_bytes = memory_at(source, count);
        std::uint8_t* destination_bytes = memory_at(destination, count);
        if (count != 0)
          std::memmove(destination_bytes, source_bytes, count);
        break;
      }
      case 11: { // memory.fill memidx
        std::uint32_t memory_index = 0;
        if (!body_uleb(body, size, &pc, &memory_index) || memory_index != 0)
          trap("memory.fill has a malformed immediate");
        const std::uint32_t count = as_u32(pop());
        const std::uint32_t value = as_u32(pop());
        const std::uint32_t destination = as_u32(pop());
        if (count != 0)
          std::memset(memory_at(destination, count), static_cast<int>(value & 0xff), count);
        else
          memory_at(destination, 0);
        break;
      }
      case 12: { // table.init elemidx tableidx
        std::uint32_t element_index = 0;
        std::uint32_t table_index = 0;
        if (!body_uleb(body, size, &pc, &element_index) ||
            !body_uleb(body, size, &pc, &table_index))
          trap("table.init has a malformed immediate");
        const std::uint32_t count = as_u32(pop());
        const std::uint32_t source = as_u32(pop());
        const std::uint32_t destination = as_u32(pop());
        Table& table = table_at(table_index);
        if (element_index >= module_.elements.size())
          trap("table.init names an element segment that does not exist");
        const ElementSegment& segment = module_.elements[element_index];
        if (count != 0 && element_dropped_[element_index])
          trap("table.init names a dropped element segment");
        if (static_cast<std::uint64_t>(source) + count > segment.refs.size())
          trap("table.init source is outside the element segment");
        if (static_cast<std::uint64_t>(destination) + count > table.entries.size())
          trap("table.init destination is outside the table");
        for (std::uint32_t i = 0; i < count; ++i)
          table.entries[destination + i] = segment.refs[source + i];
        break;
      }
      case 13: { // elem.drop
        std::uint32_t element_index = 0;
        if (!body_uleb(body, size, &pc, &element_index) || element_index >= module_.elements.size())
          trap("elem.drop names an element segment that does not exist");
        element_dropped_[element_index] = 1;
        break;
      }
      case 14: { // table.copy dsttable srctable
        std::uint32_t destination_table = 0;
        std::uint32_t source_table = 0;
        if (!body_uleb(body, size, &pc, &destination_table) ||
            !body_uleb(body, size, &pc, &source_table))
          trap("table.copy has a malformed immediate");
        const std::uint32_t count = as_u32(pop());
        const std::uint32_t source = as_u32(pop());
        const std::uint32_t destination = as_u32(pop());
        Table& to = table_at(destination_table);
        Table& from = table_at(source_table);
        if (static_cast<std::uint64_t>(source) + count > from.entries.size() ||
            static_cast<std::uint64_t>(destination) + count > to.entries.size())
          trap("table.copy is outside the table");
        if (count != 0)
          std::memmove(to.entries.data() + destination, from.entries.data() + source,
                       static_cast<std::size_t>(count) * sizeof(std::uint32_t));
        break;
      }
      case 15: { // table.grow tableidx
        std::uint32_t table_index = 0;
        if (!body_uleb(body, size, &pc, &table_index))
          trap("table.grow has a malformed immediate");
        Table& table = table_at(table_index);
        const Value initial = pop();
        if (initial.type != ValType::FuncRef)
          trap("table.grow initial value is not a funcref");
        const std::uint32_t delta = as_u32(pop());
        const std::uint32_t old_size = static_cast<std::uint32_t>(table.entries.size());
        if (static_cast<std::uint64_t>(old_size) + delta > table.maximum) {
          push_i32(0xffffffffu);
          break;
        }
        table.entries.resize(static_cast<std::size_t>(old_size) + delta, as_u32(initial));
        push_i32(old_size);
        break;
      }
      case 16: { // table.size tableidx
        std::uint32_t table_index = 0;
        if (!body_uleb(body, size, &pc, &table_index))
          trap("table.size has a malformed immediate");
        push_i32(static_cast<std::uint32_t>(table_at(table_index).entries.size()));
        break;
      }
      case 17: { // table.fill tableidx
        std::uint32_t table_index = 0;
        if (!body_uleb(body, size, &pc, &table_index))
          trap("table.fill has a malformed immediate");
        Table& table = table_at(table_index);
        const std::uint32_t count = as_u32(pop());
        const Value value = pop();
        if (value.type != ValType::FuncRef)
          trap("table.fill value is not a funcref");
        const std::uint32_t destination = as_u32(pop());
        if (static_cast<std::uint64_t>(destination) + count > table.entries.size())
          trap("table.fill is outside the table");
        std::fill(table.entries.begin() + destination, table.entries.begin() + destination + count,
                  as_u32(value));
        break;
      }
      default:
        trap("0xfc subopcode " + std::to_string(sub) + " is not hosted");
      }
      break;
    }
    default: {
      char text[64];
      std::snprintf(text, sizeof(text), "unsupported opcode 0x%02x", opcode);
      trap(text);
    }
    }
    frames_.back().pc = pc;
  }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool read_bytes(const std::string& path, std::vector<std::uint8_t>* out, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot read " + path;
    return false;
  }
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size < 0) {
    *error = "cannot read " + path;
    return false;
  }
  out->resize(static_cast<std::size_t>(size));
  if (!out->empty())
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  if (!file && !out->empty()) {
    *error = "cannot read " + path;
    return false;
  }
  return true;
}

const char* status_of(const RunReport& report) {
  if (report.ok)
    return "completed";
  if (report.budget_exhausted)
    return "budget";
  if (!report.error.empty())
    return "refused";
  return "trapped";
}

std::string number(std::uint64_t value) {
  return std::to_string(value);
}

} // namespace

struct PreparedModule {
  ParsedModule module;
  std::uint32_t indices[3] = {};
};

constexpr const char* kLifecycleNames[3] = {"pmc_wasm_load", "pmc_wasm_enable", "pmc_wasm_disable"};

bool prepare_module(const std::filesystem::path& path, const Limits& limits, PreparedModule* out,
                    std::string* file, std::string* sha256, std::uint64_t* size,
                    std::string* error) {
  const PackageReport gate = inspect_wasm(path);
  *file = gate.module.file;
  *sha256 = gate.module.sha256;
  *size = gate.module.size;
  if (!gate.ok) {
    *error = gate.error;
    return false;
  }
  if (gate.status == "component") {
    *error = "the module is a component; this build hosts core modules only";
    return false;
  }
  if (gate.status != "hostable") {
    std::string reason = "the module is not hostable";
    for (const std::string& entry : gate.reasons) {
      reason += reason.size() > 0 ? "; " : ": ";
      reason += entry;
    }
    *error = reason;
    return false;
  }
  if (!read_bytes(gate.module.file, &out->module.bytes, error))
    return false;
  if (!parse_sections(&out->module, limits, error)) {
    *error = gate.module.file + ": " + *error;
    return false;
  }

  // The lifecycle contract: three exported entry points, each answering i32.
  for (int i = 0; i < 3; ++i) {
    const auto entry = out->module.functions_by_export.find(kLifecycleNames[i]);
    if (entry == out->module.functions_by_export.end()) {
      *error = gate.module.file + ": the module does not export " + kLifecycleNames[i];
      return false;
    }
    out->indices[i] = entry->second;
    // An imported function cannot be a lifecycle entry point in this ABI (the
    // host does not provide one), so the index names a defined function.
    if (out->indices[i] < out->module.imports.size()) {
      *error = gate.module.file + ": " + kLifecycleNames[i] + " is not a defined function";
      return false;
    }
    const FuncBody& function = out->module.funcs[out->indices[i] - out->module.imports.size()];
    const FuncType& type = out->module.types[function.type_index];
    const std::string expected = i == 0 ? "(i32)->(i32)" : "()->(i32)";
    if (signature_text(type) != expected) {
      *error = gate.module.file + ": " + kLifecycleNames[i] + " is declared " +
               signature_text(type) + " but the lifecycle ABI is " + expected;
      return false;
    }
  }
  return true;
}

RunReport run_module(const std::filesystem::path& path, const Limits& limits, Host& host) {
  const auto started = std::chrono::steady_clock::now();
  RunReport report;
  report.limits = limits;
  // Every return path below reports the wall-clock it took, refusals included.
  struct Timer {
    std::chrono::steady_clock::time_point started;
    RunReport* report;
    ~Timer() {
      report->millis =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
    }
  } timer{started, &report};

  PreparedModule prepared;
  std::string error;
  if (!prepare_module(path, limits, &prepared, &report.file, &report.sha256, &report.size,
                      &error)) {
    report.error = error;
    return report;
  }
  Machine machine(prepared.module, limits, host, &report);
  machine.set_deadline(started +
                       std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis)));
  try {
    machine.instantiate();
    report.load_result = machine.call_lifecycle(prepared.indices[0], true);
    report.ran_load = true;
    if (report.load_result != 0) {
      report.error = report.file + ": pmc_wasm_load returned " + std::to_string(report.load_result);
      return report;
    }
    report.enable_result = machine.call_lifecycle(prepared.indices[1], false);
    report.ran_enable = true;
    if (report.enable_result != 0) {
      report.error =
          report.file + ": pmc_wasm_enable returned " + std::to_string(report.enable_result);
      return report;
    }
    report.disable_result = machine.call_lifecycle(prepared.indices[2], false);
    report.ran_disable = true;
    if (report.disable_result != 0) {
      report.error =
          report.file + ": pmc_wasm_disable returned " + std::to_string(report.disable_result);
      return report;
    }
    report.ok = true;
  } catch (const Stop& stop) {
    if (stop.kind == StopKind::Refuse) {
      report.error = report.file + ": " + stop.message;
    } else {
      report.trap = stop.message;
      report.budget_exhausted = stop.kind == StopKind::Budget;
    }
  } catch (const std::bad_alloc&) {
    report.error = report.file + ": out of memory instantiating the module";
  }
  return report;
}

struct Instance::Impl {
  PreparedModule prepared;
  Limits limits;
  Host* host = nullptr;
  RunReport report;
  std::unique_ptr<Machine> machine;
  // Component lifecycle calls, split at the point a server starts and stops.
  std::vector<ExportCall> start_calls;
  std::vector<ExportCall> stop_calls;
  bool has_tick_callback = false;
  std::uint32_t tick_callback = 0;
  std::uint32_t tick_event_type = 40;
  std::uint32_t tick_event_case = 40;
  bool has_task_callback = false;
  std::uint32_t task_callback = 0;
  bool task_callback_status = false;
  bool task_callback_async = false;
  bool started = false;
  bool stopped = false;

  void note_call(const std::string& name, std::int32_t result) {
    if (name == "on-load" || name == "pmc_wasm_load") {
      report.load_result = result;
      report.ran_load = true;
    } else if (name == "on-enable" || name == "pmc_wasm_enable") {
      report.enable_result = result;
      report.ran_enable = true;
    } else if (name == "on-unload" || name == "on-disable" || name == "pmc_wasm_disable") {
      report.disable_result = result;
      report.ran_disable = true;
    }
  }

  bool call_export(const ExportCall& call, std::string* error) {
    // The callback ABI returns a core status that is not the component result;
    // the compiled-core callback ABI verifies completion by the call returning.
    const bool ignore_result = call.ignore_core_result;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis));
    machine->set_deadline(deadline);
    try {
      if (call.name == "init-plugin") {
        machine->call_void(call.function_index);
        return true;
      }
      if (call.async) {
        std::string async_error;
        if (!machine->call_async(call.function_index, call.takes_handle, &async_error,
                                 machine->new_context())) {
          *error = call.name + " returned error: " + async_error;
          report.error = report.file + ": " + *error;
          return false;
        }
        note_call(call.name, 0);
        return true;
      }
      std::int32_t result =
          machine->call_lifecycle(call.function_index, call.takes_handle, machine->new_context());
      if (ignore_result)
        result = 0;
      if (call.string_result) {
        report.returns.push_back(machine->lift_string(static_cast<std::uint32_t>(result)));
        if (call.has_post_return)
          machine->call_lifecycle(call.post_return, true, static_cast<std::uint32_t>(result));
        result = 0;
      }
      note_call(call.name, result);
      if (result != 0) {
        *error = call.name + " returned " + std::to_string(result);
        return false;
      }
      return true;
    } catch (const Stop& stop) {
      if (stop.kind == StopKind::Refuse) {
        report.error = report.file + ": " + stop.message;
      } else {
        report.trap = stop.message;
        report.budget_exhausted = stop.kind == StopKind::Budget;
      }
      *error = stop.message;
      return false;
    } catch (const std::bad_alloc&) {
      *error = "out of memory executing the module";
      return false;
    }
  }

  bool call(std::uint32_t index, bool with_argument, int which, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis));
    machine->set_deadline(deadline);
    try {
      const std::int32_t result = machine->call_lifecycle(index, with_argument);
      if (which == 0) {
        report.load_result = result;
        report.ran_load = true;
      } else if (which == 1) {
        report.enable_result = result;
        report.ran_enable = true;
      } else {
        report.disable_result = result;
        report.ran_disable = true;
      }
      if (result != 0) {
        *error = std::string(kLifecycleNames[which]) + " returned " + std::to_string(result);
        return false;
      }
      return true;
    } catch (const Stop& stop) {
      if (stop.kind == StopKind::Refuse) {
        report.error = report.file + ": " + stop.message;
      } else {
        report.trap = stop.message;
        report.budget_exhausted = stop.kind == StopKind::Budget;
      }
      *error = stop.message;
      return false;
    } catch (const std::bad_alloc&) {
      *error = "out of memory executing the module";
      return false;
    }
  }
};

Instance::Instance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Instance::~Instance() = default;
Instance::Instance(Instance&&) noexcept = default;
Instance& Instance::operator=(Instance&&) noexcept = default;

std::unique_ptr<Instance> Instance::create(const std::filesystem::path& path, const Limits& limits,
                                           Host& host, std::string* error) {
  auto impl = std::make_unique<Impl>();
  impl->limits = limits;
  impl->host = &host;
  impl->report.limits = limits;
  if (!prepare_module(path, limits, &impl->prepared, &impl->report.file, &impl->report.sha256,
                      &impl->report.size, error)) {
    return nullptr;
  }
  impl->machine = std::make_unique<Machine>(impl->prepared.module, limits, host, &impl->report);
  impl->machine->set_deadline(
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis)));
  try {
    impl->machine->instantiate();
  } catch (const Stop& stop) {
    *error = stop.message;
    return nullptr;
  } catch (const std::bad_alloc&) {
    *error = "out of memory instantiating the module";
    return nullptr;
  }
  return std::unique_ptr<Instance>(new Instance(std::move(impl)));
}

std::unique_ptr<Instance> Instance::create_component(const std::filesystem::path& path,
                                                     const Limits& limits, Host& host,
                                                     std::string* error) {
  auto impl = std::make_unique<Impl>();
  impl->limits = limits;
  impl->host = &host;
  impl->report.limits = limits;
  impl->report.kind = "component";

  PreparedComponent component;
  if (!prepare_component(path, &component, error))
    return nullptr;
  impl->report.file = component.file;
  impl->report.sha256 = component.sha256;
  impl->report.size = component.size;

  impl->has_tick_callback = component.has_tick_callback;
  impl->tick_callback = component.tick_callback;
  impl->tick_event_type = component.tick_event_type;
  impl->tick_event_case = component.tick_event_case;
  impl->has_task_callback = component.has_task_callback;
  impl->task_callback = component.task_callback;
  impl->task_callback_status = component.task_callback_status;
  impl->task_callback_async = component.task_callback_async;
  impl->prepared.module.bytes = std::move(component.bytes);
  if (!parse_sections(&impl->prepared.module, limits, error,
                      component.bindings.empty() ? nullptr : &component.bindings)) {
    *error = component.file + ": " + *error;
    return nullptr;
  }
  for (const ExportCall& call : component.calls) {
    if (call.name == "on-unload" || call.name == "on-disable")
      impl->stop_calls.push_back(call);
    else
      impl->start_calls.push_back(call);
  }

  impl->machine = std::make_unique<Machine>(impl->prepared.module, limits, host, &impl->report);
  impl->machine->set_task_callback(impl->has_task_callback, impl->task_callback,
                                   impl->task_callback_status, impl->task_callback_async);
  impl->machine->set_deadline(
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis)));
  try {
    impl->machine->instantiate();
  } catch (const Stop& stop) {
    *error = stop.message;
    return nullptr;
  } catch (const std::bad_alloc&) {
    *error = "out of memory instantiating the component";
    return nullptr;
  }
  return std::unique_ptr<Instance>(new Instance(std::move(impl)));
}

bool Instance::start(std::string* error) {
  if (impl_->started) {
    *error = "the instance is already started";
    return false;
  }
  if (!impl_->start_calls.empty()) {
    for (const ExportCall& call : impl_->start_calls) {
      if (!impl_->call_export(call, error))
        return false;
    }
    impl_->started = true;
    impl_->report.ok = true;
    return true;
  }
  if (!impl_->call(impl_->prepared.indices[0], true, 0, error))
    return false;
  impl_->started = true;
  if (!impl_->call(impl_->prepared.indices[1], false, 1, error))
    return false;
  impl_->report.ok = true;
  return true;
}

bool Instance::tick(std::int32_t tick, std::string* error) {
  if (!impl_->started || impl_->stopped)
    return true;
  const bool tick_event =
      impl_->has_tick_callback && impl_->machine->event_registered(impl_->tick_event_type);
  if (!tick_event && !impl_->has_task_callback)
    return true;

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(static_cast<std::int64_t>(impl_->limits.max_millis));
  impl_->machine->set_deadline(deadline);
  try {
    if (tick_event) {
      std::uint32_t returned_case = 0;
      std::uint32_t returned_tick = 0;
      if (!impl_->machine->call_event(impl_->tick_callback, impl_->tick_event_type,
                                      impl_->machine->new_server(), impl_->tick_event_case,
                                      static_cast<std::uint32_t>(tick), &returned_case,
                                      &returned_tick, error)) {
        impl_->report.ok = false;
        impl_->report.error = impl_->report.file + ": " + *error;
        return false;
      }
      if (returned_case != impl_->tick_event_case) {
        impl_->report.ok = false;
        *error = "handle-event returned event case " + std::to_string(returned_case) +
                 ", expected " + std::to_string(impl_->tick_event_case);
        impl_->report.error = impl_->report.file + ": " + *error;
        return false;
      }
    }
    if (impl_->has_task_callback &&
        !impl_->machine->run_due_tasks(static_cast<std::uint64_t>(tick), error)) {
      impl_->report.ok = false;
      impl_->report.error = impl_->report.file + ": " + *error;
      return false;
    }
    return true;
  } catch (const Stop& stop) {
    impl_->report.ok = false;
    if (stop.kind == StopKind::Refuse) {
      impl_->report.error = impl_->report.file + ": " + stop.message;
    } else {
      impl_->report.trap = stop.message;
      impl_->report.budget_exhausted = stop.kind == StopKind::Budget;
    }
    *error = stop.message;
    return false;
  } catch (const std::bad_alloc&) {
    impl_->report.ok = false;
    *error = "out of memory executing the tick handler";
    return false;
  }
}

bool Instance::stop(std::string* error) {
  if (!impl_->started || impl_->stopped)
    return true;
  impl_->stopped = true;
  if (!impl_->stop_calls.empty()) {
    bool ok = true;
    for (const ExportCall& call : impl_->stop_calls) {
      if (!impl_->call_export(call, error))
        ok = false;
    }
    return ok;
  }
  return impl_->call(impl_->prepared.indices[2], false, 2, error);
}

const RunReport& Instance::report() const {
  return impl_->report;
}

std::string run_report_text(const RunReport& report) {
  std::string out = "+MC wasm run\n";
  if (!report.file.empty()) {
    out += report.kind + ": " + report.file + "  " + number(report.size) + " bytes  sha256 " +
           report.sha256 + "\n";
  }
  out += std::string("status: ") + status_of(report) + "\n";
  if (!report.error.empty())
    out += "error: " + report.error + "\n";
  if (!report.trap.empty()) {
    out += "trap: " + report.trap;
    if (report.budget_exhausted)
      out += " (budget)";
    out += "\n";
  }
  out += "steps: " + number(report.steps) + "  peak memory: " + number(report.peak_pages) +
         " page(s)  peak depth: " + number(report.peak_depth) +
         "  millis: " + number(report.millis) + "\n";
  out += "lifecycle: ";
  out += report.ran_load ? "load=" + std::to_string(report.load_result) : "load=<not called>";
  out += " ";
  out +=
      report.ran_enable ? "enable=" + std::to_string(report.enable_result) : "enable=<not called>";
  out += " ";
  out += report.ran_disable ? "disable=" + std::to_string(report.disable_result)
                            : "disable=<not called>";
  out += "\n";
  if (!report.host_calls.empty()) {
    out += "host calls:\n";
    for (const std::string& line : report.host_calls)
      out += "  " + line + "\n";
  }
  if (!report.log.empty()) {
    out += "log:\n";
    for (const std::string& line : report.log)
      out += "  " + line + "\n";
  }
  if (!report.messages.empty()) {
    out += "messages:\n";
    for (const std::string& line : report.messages)
      out += "  " + line + "\n";
  }
  if (!report.returns.empty()) {
    out += "returns:\n";
    for (const std::string& line : report.returns)
      out += "  " + line + "\n";
  }
  return out;
}

std::string run_report_json(const RunReport& report) {
  std::string out = "{\"tool\":\"plusmc-wasm\",\"schema\":1,\"mode\":\"run\",\"ok\":";
  out += report.ok ? "true" : "false";
  out += ",\"status\":";
  out += detail::json_quote(status_of(report));
  out += ",\"kind\":" + detail::json_quote(report.kind);
  out += ",\"module\":{\"file\":";
  out += detail::json_quote(report.file);
  out += ",\"sha256\":";
  out += detail::json_quote(report.sha256);
  out += ",\"size\":" + number(report.size) + "}";
  out += ",\"limits\":{\"steps\":" + number(report.limits.max_steps);
  out += ",\"pages\":" + number(report.limits.max_memory_pages);
  out += ",\"depth\":" + number(report.limits.max_call_depth);
  out += ",\"millis\":" + number(report.limits.max_millis);
  out += ",\"host_bytes\":" + number(report.limits.max_host_bytes) + "}";
  out += ",\"steps\":" + number(report.steps);
  out += ",\"peak_pages\":" + number(report.peak_pages);
  out += ",\"peak_depth\":" + number(report.peak_depth);
  out += ",\"budget_exhausted\":";
  out += report.budget_exhausted ? "true" : "false";
  out += ",\"error\":";
  out += detail::json_quote(report.error);
  out += ",\"trap\":";
  out += detail::json_quote(report.trap);
  out += ",\"lifecycle\":{\"load\":{\"ran\":";
  out += report.ran_load ? "true" : "false";
  out += ",\"result\":" + std::to_string(report.load_result) + "},\"enable\":{\"ran\":";
  out += report.ran_enable ? "true" : "false";
  out += ",\"result\":" + std::to_string(report.enable_result) + "},\"disable\":{\"ran\":";
  out += report.ran_disable ? "true" : "false";
  out += ",\"result\":" + std::to_string(report.disable_result) + "}}";
  out += ",\"host_calls\":";
  detail::json_list(&out, report.host_calls, kMaxReportedHostCalls);
  out += ",\"log\":";
  detail::json_list(&out, report.log);
  out += ",\"messages\":";
  detail::json_list(&out, report.messages);
  out += ",\"returns\":";
  detail::json_list(&out, report.returns);
  return out + "}\n";
}

RunReport run_core_in_memory(const std::vector<std::uint8_t>& bytes, const std::string& label,
                             const std::vector<ExportCall>& calls, const Limits& limits, Host& host,
                             const std::vector<CoreImportBinding>& bindings) {
  const auto started = std::chrono::steady_clock::now();
  RunReport report;
  report.limits = limits;
  report.file = label;
  struct Timer {
    std::chrono::steady_clock::time_point started;
    RunReport* report;
    ~Timer() {
      report->millis =
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count());
    }
  } timer{started, &report};

  ParsedModule module;
  module.bytes = bytes;
  std::string error;
  if (!parse_sections(&module, limits, &error, bindings.empty() ? nullptr : &bindings)) {
    report.error = label + ": " + error;
    return report;
  }

  // Resolve and check every call before anything executes, so a component with
  // one bad entry point is refused rather than half-run.
  for (const ExportCall& call : calls) {
    if (call.function_index >= module.function_count()) {
      report.error = label + ": " + call.name + " names a function that does not exist";
      return report;
    }
    if (call.function_index < module.imports.size()) {
      report.error =
          label + ": " + call.name + " is an imported function, not one the module defines";
      return report;
    }
    const FuncBody& function = module.funcs[call.function_index - module.imports.size()];
    const FuncType& type = module.types[function.type_index];
    if (call.string_result) {
      if (call.async || call.takes_handle) {
        report.error = label + ": " + call.name +
                       " is a lifted string result and cannot be async or take a handle";
        return report;
      }
      if (!type.params.empty() || !type.has_result || type.result != ValType::I32) {
        report.error = label + ": " + call.name + " is declared " + signature_text(type) +
                       " but a lifted string result is ()->(i32)";
        return report;
      }
      if (!module.has_memory) {
        report.error = label + ": " + call.name + " returns a string but the module has no memory";
        return report;
      }
      if (call.has_post_return) {
        if (call.post_return >= module.function_count()) {
          report.error =
              label + ": " + call.name + " names a post-return function that does not exist";
          return report;
        }
        if (call.post_return < module.imports.size()) {
          report.error = label + ": " + call.name + " names an imported post-return function";
          return report;
        }
        const FuncBody& post = module.funcs[call.post_return - module.imports.size()];
        const FuncType& post_type = module.types[post.type_index];
        if (post_type.params.size() != 1 || post_type.params[0] != ValType::I32 ||
            post_type.has_result) {
          report.error = label + ": " + call.name + " names a post-return function declared " +
                         signature_text(post_type) + ", not (i32)->()";
          return report;
        }
      }
    } else if (call.async) {
      const std::size_t expected = call.takes_handle ? 1 : 0;
      if (type.params.size() != expected || type.has_result) {
        report.error = label + ": " + call.name + " is not the async " +
                       (call.takes_handle ? "(i32)->()" : "()->()") + " shape (declared " +
                       signature_text(type) + ")";
        return report;
      }
    } else if (call.no_result) {
      const std::size_t expected = call.takes_handle ? 1 : 0;
      if (type.params.size() != expected || type.has_result) {
        report.error = label + ": " + call.name + " is declared " + signature_text(type) +
                       " but the lifecycle hook is " + (call.takes_handle ? "(i32)->()" : "()->()");
        return report;
      }
    } else if (call.ignore_core_result) {
      const std::size_t expected = call.takes_handle ? 1 : 0;
      if (type.params.size() != expected || (type.has_result && type.result != ValType::I32)) {
        report.error = label + ": " + call.name +
                       " is not the callback lifecycle (i32)->(i32) shape (declared " +
                       signature_text(type) + ")";
        return report;
      }
    } else if (!type.params.empty()) {
      report.error = label + ": " + call.name +
                     " takes parameters; this ABI calls a lifecycle entry with no arguments";
      return report;
    }
  }

  Machine machine(module, limits, host, &report);
  machine.set_deadline(started +
                       std::chrono::milliseconds(static_cast<std::int64_t>(limits.max_millis)));
  try {
    machine.instantiate();
    for (const ExportCall& call : calls) {
      if (call.name == "init-plugin") {
        machine.call_void(call.function_index);
        continue;
      }
      std::int32_t result = 0;
      if (call.async) {
        std::string async_error;
        if (!machine.call_async(call.function_index, call.takes_handle, &async_error,
                                machine.new_context())) {
          report.error = label + ": " + call.name + " returned error: " + async_error;
          return report;
        }
      } else {
        result =
            machine.call_lifecycle(call.function_index, call.takes_handle, machine.new_context());
        if (call.ignore_core_result)
          result = 0;
        if (call.string_result) {
          const std::string value = machine.lift_string(static_cast<std::uint32_t>(result));
          report.returns.push_back(value);
          if (call.has_post_return)
            machine.call_lifecycle(call.post_return, true, static_cast<std::uint32_t>(result));
          result = 0;
        }
      }
      if (call.name == "on-load" || call.name == "pmc_wasm_load") {
        report.load_result = result;
        report.ran_load = true;
      } else if (call.name == "on-enable" || call.name == "pmc_wasm_enable") {
        report.enable_result = result;
        report.ran_enable = true;
      } else if (call.name == "on-unload" || call.name == "on-disable" ||
                 call.name == "pmc_wasm_disable") {
        report.disable_result = result;
        report.ran_disable = true;
      }
      if (!call.async && result != 0) {
        report.error = label + ": " + call.name + " returned " + std::to_string(result);
        return report;
      }
    }
    report.ok = true;
  } catch (const Stop& stop) {
    if (stop.kind == StopKind::Refuse) {
      report.error = label + ": " + stop.message;
    } else {
      report.trap = stop.message;
      report.budget_exhausted = stop.kind == StopKind::Budget;
    }
  } catch (const std::bad_alloc&) {
    report.error = label + ": out of memory instantiating the module";
  }
  return report;
}

} // namespace mc::wasm
