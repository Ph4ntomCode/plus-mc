// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bounded reader behind `plusmc-wasm inspect`. WebAssembly's binary format
// is a versioned header followed by sections; this file walks that structure
// and nothing else. It never runs code, allocates from a length claim, or
// trusts a count: every vector is capped and every section is checked against
// the bytes that remain.
//
// Core modules are parsed far enough to list imports and exports, because that
// is what decides whether the pinned host can satisfy them. Components get a
// header and section-table check; the component layer's imports live in a
// different encoding and are not claimed here (slice 2 hosts them).

#include "wasm/module.h"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "converter/report_json.h"

namespace mc::wasm {

// The JSON helpers are shared with the converter; the alias keeps the emitted
// escaping identical without exposing the converter's namespace to callers.
namespace detail = mc::converter::detail;

const std::vector<HostFunctionRef>& host_surface() {
  static const std::vector<HostFunctionRef> kSurface = {
      {"broadcast", "(i32,i32)->()"},
      {"config_get", "(i32,i32,i32,i32)->(i32)"},
      // The canonical-ABI form of config_get: a (key: string) -> string lower
      // with memory and realloc options. It is only bound through a canon
      // lower; a direct core import has no realloc and is refused at run time.
      {"config_get_string", "(i32,i32)->(i32)"},
      // The canonical-ABI list form: () -> list<string>, flattened to one
      // return-area pointer and allocated through the module's cabi_realloc.
      {"config_keys", "()->(i32)"},
      // The canonical-ABI record form: (key: string) -> record { key: string,
      // value: string }, flattened to (i32,i32)->(i32) and allocated through
      // the module's cabi_realloc.
      {"config_pair", "(i32,i32)->(i32)"},
      {"config_set", "(i32,i32,i32,i32)->(i32)"},
      {"entity_find", "(i32,i32)->(i32)"},
      {"log", "(i32,i32)->()"},
      {"log_level", "(i32,i32,i32)->()"},
      // Pumpkin's `log-tracing(event: list<u8>)`: a (pointer, length) byte array
      // in the module's memory 0.
      {"log_tracing", "(i32,i32)->()"},
      {"player_count", "()->(i32)"},
      // Pumpkin's `scheduler` interface: task ids are per plugin instance, and
      // the tick pump drives the matching `handle-task` export.
      {"scheduler_schedule_delayed", "(i32,i64)->(i32)"},
      {"scheduler_schedule_repeating", "(i32,i64,i64)->(i32)"},
      {"scheduler_cancel", "(i32)->()"},
      {"player_chat", "(i32,i32)->()"},
      // Pumpkin's `player.send-system-message(text-component, overlay)`. The
      // text argument is a typed host resource, so the call is handle, text
      // handle, and the overlay bool.
      {"player_send_system_message", "(i32,i32,i32)->()"},
      // Pumpkin's `server.has-world(name: string) -> bool`.
      {"server_has_world", "(i32,i32,i32)->(i32)"},
      // Pumpkin's scalar `server` settings, each a method on the server handle.
      {"server_max_players", "(i32)->(i32)"},
      {"server_view_distance", "(i32)->(i32)"},
      {"server_simulation_distance", "(i32)->(i32)"},
      {"server_default_gamemode", "(i32)->(i32)"},
      {"server_difficulty", "(i32)->(i32)"},
      {"server_online_mode", "(i32)->(i32)"},
      {"server_whitelist_enabled", "(i32)->(i32)"},
      // Pumpkin's `world.get-block-id(pos: block-pos) -> u16`, the block type
      // id beside the state id already exposed by world_get_block.
      {"world_get_block_id", "(i32,i32,i32,i32)->(i32)"},
      // Pumpkin's `world.set-time-of-day(ticks: u64)`.
      {"world_set_time", "(i32,i64)->()"},
      // Pumpkin's `server.get-player-by-name(name: string) -> option<player>`;
      // the guest supplies the 8-byte option return area.
      {"server_player_by_name", "(i32,i32,i32,i32)->()"},
      // Pumpkin's `server.broadcast(message)`; the implicit server handle leads.
      {"server_broadcast", "(i32,i32,i32)->()"},
      // The text resource: plain construction and append, plus the style and
      // click/hover builders that carry a value in the host's text tree.
      {"text_text", "(i32,i32)->(i32)"},
      {"text_add_text", "(i32,i32,i32)->()"},
      {"text_add_child", "(i32,i32)->()"},
      {"text_color_named", "(i32,i32)->()"},
      {"text_color_rgb", "(i32,i32,i32,i32)->()"},
      {"text_bold", "(i32,i32)->()"},
      {"text_italic", "(i32,i32)->()"},
      {"text_underlined", "(i32,i32)->()"},
      {"text_strikethrough", "(i32,i32)->()"},
      {"text_obfuscated", "(i32,i32)->()"},
      {"text_insertion", "(i32,i32,i32)->()"},
      {"text_font", "(i32,i32,i32)->()"},
      {"text_click_open_url", "(i32,i32,i32)->()"},
      {"text_click_open_file", "(i32,i32,i32)->()"},
      {"text_click_run_command", "(i32,i32,i32)->()"},
      {"text_click_suggest_command", "(i32,i32,i32)->()"},
      {"text_click_change_page", "(i32,i32)->()"},
      {"text_click_copy_to_clipboard", "(i32,i32,i32)->()"},
      {"text_hover_show_text", "(i32,i32)->()"},
      // Compiler-produced resource methods carry the implicit `self` handle as
      // the first core parameter; the compiled-core path binds that shape to
      // the same player-chat sink. `player_get_name` takes the handle and the
      // guest's two-word string return area.
      {"player_get_name", "(i32,i32)->()"},
      {"player_get_position", "(i32,i32)->()"},
      {"player_get_yaw", "(i32)->(f32)"},
      {"player_get_pitch", "(i32)->(f32)"},
      {"pumpkin_player_chat", "(i32,i32,i32)->()"},
      {"player_message", "(i32,i32)->()"},
      {"reply", "(i32,i32)->()"},
      {"world_get_block", "(i32,i32,i32)->(i32)"},
      // The canonical-ABI form of world.get-block: the 56-byte `block` record
      // is returned through a pointer allocated with the module's cabi_realloc.
      {"world_get_block_info", "(i32,i32,i32)->(i32)"},
      {"world_set_block", "(i32,i32,i32,i32)->(i32)"},
      // Pumpkin's async `world.set-block`: pos (three i32), block record
      // pointer, update flags. The host applies the record's default state and
      // completes the call synchronously.
      {"pumpkin_world_set_block", "(i32,i32,i32,i32,i32)->()"},
      // The compiled async lower spills the whole parameter area behind one
      // in-pointer; the runtime reads that canonical area.
      {"pumpkin_world_set_block_spill", "(i32)->()"},
      {"world_time", "()->(i64)"},
      {"world_weather", "()->(i32)"},
      {"item_stack_new", "(i32,i32,i32)->(i32)"},
      {"item_stack_get_registry_key", "(i32,i32)->()"},
      {"item_stack_get_count", "(i32)->(i32)"},
      {"item_stack_set_count", "(i32,i32)->()"},
      {"item_stack_get_max_count", "(i32)->(i32)"},
      {"item_stack_clone", "(i32)->(i32)"},
      {"item_stack_with_count", "(i32,i32)->(i32)"},
      {"item_stack_is_similar", "(i32,i32)->(i32)"},
      {"item_stack_get_damage", "(i32)->(i32)"},
      {"item_stack_set_damage", "(i32,i32)->()"},
      {"item_stack_get_max_damage", "(i32)->(i32)"},
      {"item_stack_is_damageable", "(i32)->(i32)"},
      {"item_stack_is_unbreakable", "(i32)->(i32)"},
      {"item_stack_set_unbreakable", "(i32,i32)->()"},
      {"item_stack_get_repair_cost", "(i32)->(i32)"},
      {"item_stack_set_repair_cost", "(i32,i32)->()"},
      {"item_stack_get_item_model", "(i32,i32)->()"},
      {"item_stack_set_item_model", "(i32,i32,i32,i32)->()"},
      {"item_stack_get_enchantment_glint_override", "(i32,i32)->()"},
      {"item_stack_set_enchantment_glint_override", "(i32,i32,i32)->()"},
      {"item_stack_get_attribute_modifiers", "(i32,i32)->()"},
      {"item_stack_add_attribute_modifier", "(i32,i32,i32,i32,f64,i32,i32)->()"},
      {"item_stack_remove_attribute_modifiers", "(i32,i32)->()"},
      {"item_stack_clear_attribute_modifiers", "(i32)->()"},
      {"item_stack_set_custom_data", "(i32,i32,i32,i32,i32,i32,i32,i32)->()"},
      {"item_stack_get_custom_data", "(i32,i32,i32,i32,i32,i32)->()"},
      {"item_stack_remove_custom_data", "(i32,i32,i32,i32,i32)->()"},
      {"item_stack_has_custom_data", "(i32,i32,i32,i32,i32)->(i32)"},
      {"item_stack_get_components", "(i32,i32)->()"},
      {"item_stack_set_component", "(i32,i32,i32,i32)->()"},
      {"item_stack_remove_component", "(i32,i32)->()"},
      {"item_stack_get_enchantments", "(i32,i32)->()"},
      {"item_stack_add_enchantment", "(i32,i32,i32)->()"},
      {"item_stack_remove_enchantment", "(i32,i32)->()"},
      {"item_stack_get_custom_enchantments", "(i32,i32)->()"},
      {"item_stack_add_custom_enchantment", "(i32,i32,i32,i32)->()"},
      {"item_stack_remove_custom_enchantment", "(i32,i32,i32)->()"},
      {"item_stack_get_custom_enchantment_level", "(i32,i32,i32,i32)->()"},
      {"item_stack_has_custom_enchantment", "(i32,i32,i32)->(i32)"},
      {"item_stack_get_lore", "(i32,i32)->()"},
      {"item_stack_set_lore", "(i32,i32,i32)->()"},
      {"item_stack_add_lore", "(i32,i32)->()"},
      {"item_stack_get_custom_name", "(i32,i32)->()"},
      {"item_stack_set_custom_name", "(i32,i32,i32)->()"},
      {"resource_drop_item", "(i32)->()"},
      {"resource_drop_player", "(i32)->()"},
      {"resource_drop_text", "(i32)->()"},
  };
  return kSurface;
}

bool known_host_function(const std::string& name) {
  for (const HostFunctionRef& entry : host_surface()) {
    if (name == entry.name)
      return true;
  }
  return false;
}

namespace {

constexpr std::size_t kMaxModuleBytes = 64u * 1024u * 1024u;
constexpr std::size_t kMaxManifestBytes = 64u * 1024u;
constexpr std::size_t kMaxSections = 4096;
constexpr std::size_t kMaxTypes = 100000;
constexpr std::size_t kMaxImports = 100000;
constexpr std::size_t kMaxExports = 100000;
constexpr std::size_t kMaxNameBytes = 4096;
constexpr std::size_t kMaxDisplayedEntries = 1000;

constexpr std::uint8_t kSectionCustom = 0;
constexpr std::uint8_t kSectionType = 1;
constexpr std::uint8_t kSectionImport = 2;
constexpr std::uint8_t kSectionFunction = 3;
constexpr std::uint8_t kSectionTable = 4;
constexpr std::uint8_t kSectionMemory = 5;
constexpr std::uint8_t kSectionGlobal = 6;
constexpr std::uint8_t kSectionExport = 7;
constexpr std::uint8_t kSectionStart = 8;
constexpr std::uint8_t kSectionElement = 9;
constexpr std::uint8_t kSectionCode = 10;
constexpr std::uint8_t kSectionData = 11;
constexpr std::uint8_t kSectionDataCount = 12;
constexpr std::uint8_t kSectionTag = 13;

// The spec's section order is not id order: DataCount (12) precedes Code (10).
int section_order(std::uint8_t id) {
  switch (id) {
  case kSectionType:
    return 1;
  case kSectionImport:
    return 2;
  case kSectionFunction:
    return 3;
  case kSectionTable:
    return 4;
  case kSectionMemory:
    return 5;
  case kSectionGlobal:
    return 6;
  case kSectionExport:
    return 7;
  case kSectionStart:
    return 8;
  case kSectionElement:
    return 9;
  case kSectionDataCount:
    return 10;
  case kSectionCode:
    return 11;
  case kSectionData:
    return 12;
  case kSectionTag:
    return 13;
  default:
    return -1;
  }
}

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
    for (unsigned int shift = 0; shift < 35; shift += 7) {
      std::uint8_t byte = 0;
      if (!u8(&byte))
        return false;
      if (shift == 28 && (byte & 0xf0) != 0)
        return false; // more than 32 bits of LEB128
      result |= static_cast<std::uint32_t>(byte & 0x7f) << shift;
      if ((byte & 0x80) == 0) {
        *out = result;
        return true;
      }
    }
    return false;
  }

  bool name(std::string* out) {
    std::uint32_t length = 0;
    if (!u32(&length))
      return false;
    if (length > kMaxNameBytes)
      return false;
    if (length > remaining())
      return false;
    const char* start = reinterpret_cast<const char*>(data_ + position_);
    if (!valid_utf8(start, length))
      return false;
    out->assign(start, length);
    position_ += length;
    return true;
  }

  bool skip(std::size_t count) {
    if (count > remaining())
      return false;
    position_ += count;
    return true;
  }

  std::size_t remaining() const {
    return size_ - position_;
  }
  std::size_t position() const {
    return position_;
  }

  // A section payload is a sub-reader; its length was already bounded against
  // the outer buffer, so a malformed inner count cannot escape it.
  bool sub(std::uint32_t length, Reader* out) const {
    if (length > remaining())
      return false;
    *out = Reader(data_ + position_, length);
    return true;
  }

  void advance(std::uint32_t length) {
    position_ += length;
  }

private:
  static bool valid_utf8(const char* bytes, std::size_t length) {
    std::size_t i = 0;
    while (i < length) {
      const unsigned char c = static_cast<unsigned char>(bytes[i]);
      if (c == 0)
        return false;
      std::size_t follow = 0;
      if (c < 0x80) {
        ++i;
        continue;
      } else if ((c & 0xe0) == 0xc0) {
        follow = 1;
        if (c < 0xc2)
          return false; // overlong
      } else if ((c & 0xf0) == 0xe0) {
        follow = 2;
      } else if ((c & 0xf8) == 0xf0) {
        follow = 3;
        if (c > 0xf4)
          return false;
      } else {
        return false;
      }
      if (i + follow >= length)
        return false;
      for (std::size_t j = 1; j <= follow; ++j) {
        if ((static_cast<unsigned char>(bytes[i + j]) & 0xc0) != 0x80)
          return false;
      }
      i += follow + 1;
    }
    return true;
  }

  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t position_ = 0;
};

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr)
    return {};
  const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(context, data, size) == 1 &&
                  EVP_DigestFinal_ex(context, digest.data(), &length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok || length != 32)
    return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned int i = 0; i < length; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

bool read_file(const std::filesystem::path& path, std::size_t cap, std::vector<std::uint8_t>* out,
               std::string* error) {
  std::error_code status_error;
  const std::uintmax_t size = std::filesystem::file_size(path, status_error);
  if (status_error) {
    *error = "cannot read " + path.generic_string() + ": " + status_error.message();
    return false;
  }
  if (size > cap) {
    *error = path.generic_string() + " is larger than the " + std::to_string(cap) + "-byte limit";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  out->resize(static_cast<std::size_t>(size));
  if (!out->empty())
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  if (!file && !out->empty()) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  return true;
}

std::string number(std::size_t value) {
  return std::to_string(value);
}

std::string valtype_name(std::uint8_t byte) {
  switch (byte) {
  case 0x7f:
    return "i32";
  case 0x7e:
    return "i64";
  case 0x7d:
    return "f32";
  case 0x7c:
    return "f64";
  case 0x7b:
    return "v128";
  case 0x70:
    return "funcref";
  case 0x6f:
    return "externref";
  default:
    return {};
  }
}

std::string kind_name(std::uint8_t kind) {
  switch (kind) {
  case 0:
    return "function";
  case 1:
    return "table";
  case 2:
    return "memory";
  case 3:
    return "global";
  case 4:
    return "tag";
  default:
    return {};
  }
}

std::string signature_text(const std::vector<std::string>& params,
                           const std::vector<std::string>& results) {
  std::string out = "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0)
      out += ",";
    out += params[i];
  }
  out += ")->(";
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (i != 0)
      out += ",";
    out += results[i];
  }
  return out + ")";
}

bool parse_valtype(Reader* reader, std::string* error) {
  std::uint8_t byte = 0;
  if (!reader->u8(&byte)) {
    *error = "a value type runs past the section";
    return false;
  }
  if (valtype_name(byte).empty()) {
    char text[64];
    std::snprintf(text, sizeof(text), "unsupported value type 0x%02x", byte);
    *error = text;
    return false;
  }
  return true;
}

bool parse_limits(Reader* reader, std::string* error) {
  std::uint8_t flags = 0;
  if (!reader->u8(&flags)) {
    *error = "a limits descriptor runs past the section";
    return false;
  }
  if (flags > 1) {
    *error = "unsupported limits flags (64-bit or shared memory)";
    return false;
  }
  std::uint32_t value = 0;
  if (!reader->u32(&value)) {
    *error = "a limits minimum runs past the section";
    return false;
  }
  if (flags == 1 && !reader->u32(&value)) {
    *error = "a limits maximum runs past the section";
    return false;
  }
  return true;
}

bool parse_table_type(Reader* reader, std::string* error) {
  std::uint8_t reference = 0;
  if (!reader->u8(&reference)) {
    *error = "a table type runs past the section";
    return false;
  }
  if (reference != 0x70 && reference != 0x6f) {
    *error = "unsupported table reference type";
    return false;
  }
  return parse_limits(reader, error);
}

bool parse_global_type(Reader* reader, std::string* error) {
  if (!parse_valtype(reader, error))
    return false;
  std::uint8_t mutability = 0;
  if (!reader->u8(&mutability)) {
    *error = "a global type runs past the section";
    return false;
  }
  if (mutability > 1) {
    *error = "unsupported global mutability";
    return false;
  }
  return true;
}

struct TypeSignature {
  std::vector<std::string> params;
  std::vector<std::string> results;
};

bool parse_type_section(Reader reader, ModuleInfo* module, std::vector<TypeSignature>* types,
                        std::string* error) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    *error = "the type section has no count";
    return false;
  }
  if (count > kMaxTypes) {
    *error = "the type section claims more than " + std::to_string(kMaxTypes) + " types";
    return false;
  }
  types->reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint8_t form = 0;
    if (!reader.u8(&form)) {
      *error = "type " + std::to_string(index) + " runs past the section";
      return false;
    }
    if (form != 0x60) {
      char text[80];
      std::snprintf(text, sizeof(text),
                    "type %u uses unsupported form 0x%02x (not a function type)", index, form);
      *error = text;
      return false;
    }
    TypeSignature signature;
    std::uint32_t params = 0;
    if (!reader.u32(&params) || params > kMaxTypes) {
      *error = "type " + std::to_string(index) + " has an implausible parameter count";
      return false;
    }
    for (std::uint32_t i = 0; i < params; ++i) {
      std::uint8_t byte = 0;
      if (!reader.u8(&byte) || valtype_name(byte).empty()) {
        *error = "type " + std::to_string(index) + " has an unsupported parameter type";
        return false;
      }
      signature.params.push_back(valtype_name(byte));
    }
    std::uint32_t results = 0;
    if (!reader.u32(&results) || results > kMaxTypes) {
      *error = "type " + std::to_string(index) + " has an implausible result count";
      return false;
    }
    for (std::uint32_t i = 0; i < results; ++i) {
      std::uint8_t byte = 0;
      if (!reader.u8(&byte) || valtype_name(byte).empty()) {
        *error = "type " + std::to_string(index) + " has an unsupported result type";
        return false;
      }
      signature.results.push_back(valtype_name(byte));
    }
    types->push_back(std::move(signature));
  }
  module->type_count = count;
  return true;
}

bool parse_import_section(Reader reader, ModuleInfo* module,
                          const std::vector<TypeSignature>& types, std::string* error) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    *error = "the import section has no count";
    return false;
  }
  if (count > kMaxImports) {
    *error = "the import section claims more than " + std::to_string(kMaxImports) + " imports";
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ImportRef import;
    std::uint8_t kind = 0;
    if (!reader.name(&import.module) || !reader.name(&import.name) || !reader.u8(&kind)) {
      *error = "import " + std::to_string(index) + " runs past the section";
      return false;
    }
    import.kind = kind_name(kind);
    if (import.kind.empty()) {
      *error = "import " + std::to_string(index) + " has unknown kind " + std::to_string(kind);
      return false;
    }
    switch (kind) {
    case 0: { // function
      std::uint32_t type_index = 0;
      if (!reader.u32(&type_index)) {
        *error = "import " + std::to_string(index) + " has no type index";
        return false;
      }
      if (type_index >= types.size()) {
        *error = "import " + std::to_string(index) + " names type " + std::to_string(type_index) +
                 " but only " + std::to_string(types.size()) + " are declared";
        return false;
      }
      import.signature = signature_text(types[type_index].params, types[type_index].results);
      break;
    }
    case 1: // table
      if (!parse_table_type(&reader, error))
        return false;
      break;
    case 2: // memory
      if (!parse_limits(&reader, error))
        return false;
      break;
    case 3: // global
      if (!parse_global_type(&reader, error))
        return false;
      break;
    case 4: { // tag
      std::uint8_t attribute = 0;
      std::uint32_t type_index = 0;
      if (!reader.u8(&attribute) || !reader.u32(&type_index)) {
        *error = "a tag import runs past the section";
        return false;
      }
      if (attribute != 0 || type_index >= types.size()) {
        *error = "a tag import has an invalid attribute or type index";
        return false;
      }
      break;
    }
    default:
      break;
    }
    module->imports.push_back(std::move(import));
  }
  return true;
}

bool parse_export_section(Reader reader, ModuleInfo* module, std::string* error) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    *error = "the export section has no count";
    return false;
  }
  if (count > kMaxExports) {
    *error = "the export section claims more than " + std::to_string(kMaxExports) + " exports";
    return false;
  }
  std::set<std::string> names;
  for (std::uint32_t index = 0; index < count; ++index) {
    ExportRef export_ref;
    std::uint8_t kind = 0;
    if (!reader.name(&export_ref.name) || !reader.u8(&kind) || !reader.u32(&export_ref.index)) {
      *error = "export " + std::to_string(index) + " runs past the section";
      return false;
    }
    export_ref.kind = kind_name(kind);
    if (export_ref.kind.empty()) {
      *error = "export " + std::to_string(index) + " has unknown kind " + std::to_string(kind);
      return false;
    }
    if (!names.insert(export_ref.name).second) {
      *error = "the export \"" + export_ref.name + "\" is declared twice";
      return false;
    }
    module->exports.push_back(std::move(export_ref));
  }
  return true;
}

bool read_count(Reader* reader, std::uint32_t* count, const char* name, std::string* error) {
  if (!reader->u32(count)) {
    *error = std::string("the ") + name + " section has no count";
    return false;
  }
  if (*count > kMaxTypes) {
    *error = std::string("the ") + name + " section has an implausible count";
    return false;
  }
  return true;
}

bool parse_table_section(Reader reader, ModuleInfo* module, std::string* error) {
  std::uint32_t count = 0;
  if (!read_count(&reader, &count, "table", error))
    return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!parse_table_type(&reader, error)) {
      *error = "a table descriptor is malformed or unsupported (" + *error + ")";
      return false;
    }
  }
  module->table_count = count;
  return true;
}

bool parse_memory_section(Reader reader, ModuleInfo* module, std::string* error) {
  std::uint32_t count = 0;
  if (!read_count(&reader, &count, "memory", error))
    return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!parse_limits(&reader, error))
      return false;
  }
  module->memory_count = count;
  return true;
}

bool parse_global_section(Reader reader, ModuleInfo* module, std::string* error) {
  std::uint32_t count = 0;
  if (!read_count(&reader, &count, "global", error))
    return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!parse_global_type(&reader, error))
      return false;
  }
  module->global_count = count;
  return true;
}

bool parse_tag_section(Reader reader, ModuleInfo* module, std::string* error) {
  std::uint32_t count = 0;
  if (!read_count(&reader, &count, "tag", error))
    return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t attribute = 0;
    std::uint32_t type_index = 0;
    if (!reader.u8(&attribute) || !reader.u32(&type_index) || attribute != 0) {
      *error = "a tag descriptor is malformed";
      return false;
    }
  }
  module->tag_count = count;
  return true;
}

bool parse_core_module(const std::uint8_t* data, std::size_t size, ModuleInfo* module,
                       std::string* error) {
  Reader reader(data + 8, size - 8);
  std::vector<TypeSignature> types;
  bool seen[kSectionTag + 1] = {};
  int last_order = 0;
  std::uint32_t function_section_count = 0;
  bool saw_function_section = false;

  while (reader.remaining() != 0) {
    if (module->sections.size() >= kMaxSections) {
      *error = "more than " + std::to_string(kMaxSections) + " sections";
      return false;
    }
    const std::size_t section_start = reader.position() + 8;
    std::uint8_t id = 0;
    std::uint32_t length = 0;
    if (!reader.u8(&id) || !reader.u32(&length)) {
      *error = "a section header at byte " + number(section_start) + " runs past the file";
      return false;
    }
    if (length > reader.remaining()) {
      *error = "section " + std::to_string(id) + " at byte " + number(section_start) + " claims " +
               std::to_string(length) + " bytes but only " + std::to_string(reader.remaining()) +
               " remain";
      return false;
    }
    Reader payload(nullptr, 0);
    if (!reader.sub(length, &payload)) {
      *error = "section " + std::to_string(id) + " runs past the file";
      return false;
    }
    module->sections.push_back(SectionRef{id, length});

    if (id != kSectionCustom) {
      const int order = section_order(id);
      if (order < 0) {
        *error = "unknown section id " + std::to_string(id) + " at byte " + number(section_start);
        return false;
      }
      if (seen[id]) {
        *error = "section " + std::to_string(id) + " appears more than once";
        return false;
      }
      if (order < last_order) {
        *error = "section " + std::to_string(id) + " is out of order";
        return false;
      }
      seen[id] = true;
      last_order = order;
    }

    switch (id) {
    case kSectionCustom: {
      std::string name;
      if (!payload.name(&name)) {
        *error = "a custom section at byte " + number(section_start) + " has no valid name";
        return false;
      }
      module->notes.push_back("custom section: " + name);
      break;
    }
    case kSectionType:
      if (!parse_type_section(payload, module, &types, error))
        return false;
      break;
    case kSectionImport:
      if (!parse_import_section(payload, module, types, error))
        return false;
      break;
    case kSectionFunction: {
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the function section has an implausible count";
        return false;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t type_index = 0;
        if (!payload.u32(&type_index) || type_index >= types.size()) {
          *error = "function " + std::to_string(i) + " names an undeclared type";
          return false;
        }
      }
      module->function_count = count;
      function_section_count = count;
      saw_function_section = true;
      break;
    }
    case kSectionTable:
      if (!parse_table_section(payload, module, error))
        return false;
      break;
    case kSectionMemory:
      if (!parse_memory_section(payload, module, error))
        return false;
      break;
    case kSectionGlobal:
      if (!parse_global_section(payload, module, error))
        return false;
      break;
    case kSectionExport:
      if (!parse_export_section(payload, module, error))
        return false;
      break;
    case kSectionStart: {
      std::uint32_t index = 0;
      if (!payload.u32(&index)) {
        *error = "the start section has no function index";
        return false;
      }
      break;
    }
    case kSectionCode: {
      std::uint32_t count = 0;
      if (!payload.u32(&count) || count > kMaxTypes) {
        *error = "the code section has an implausible count";
        return false;
      }
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t body_size = 0;
        if (!payload.u32(&body_size) || !payload.skip(body_size)) {
          *error = "code body " + std::to_string(i) + " runs past the section";
          return false;
        }
      }
      module->code_count = count;
      break;
    }
    case kSectionTag:
      if (!parse_tag_section(payload, module, error))
        return false;
      break;
    case kSectionDataCount: {
      std::uint32_t count = 0;
      if (!payload.u32(&count)) {
        *error = "the data count section has no count";
        return false;
      }
      break;
    }
    case kSectionElement:
    case kSectionData:
      // The count is enough for a capability report; the payload itself was
      // bounds-checked by the section length.
      break;
    default:
      break;
    }
    reader.advance(length);
  }

  if (saw_function_section && function_section_count != module->code_count) {
    *error = "the function section declares " + std::to_string(function_section_count) +
             " functions but the code section has " + std::to_string(module->code_count);
    return false;
  }
  return true;
}

bool parse_component(const std::uint8_t* data, std::size_t size, ModuleInfo* module,
                     std::string* error) {
  Reader reader(data + 8, size - 8);
  while (reader.remaining() != 0) {
    if (module->sections.size() >= kMaxSections) {
      *error = "more than " + std::to_string(kMaxSections) + " sections";
      return false;
    }
    std::uint8_t id = 0;
    std::uint32_t length = 0;
    if (!reader.u8(&id) || !reader.u32(&length) || length > reader.remaining()) {
      *error = "a component section runs past the file";
      return false;
    }
    if (id > 12) {
      *error = "unknown component section id " + std::to_string(id);
      return false;
    }
    // Component sections are definitions, not a core module's singleton
    // section table: the spec allows any order and repeated ids (several core
    // modules, several import or export sections). Only the bounds and the
    // header are shared with a core module, so there is no order or duplicate
    // check here; slice 3's adapter reads what the sections contain.
    module->sections.push_back(SectionRef{id, length});
    reader.advance(length);
  }
  module->notes.push_back(
      "a component uses the component-model layer; `inspect` checks the header and section "
      "table only, and `plusmc-wasm-adapt` reads its imports and exports");
  return true;
}

void classify(const ModuleInfo& module, PackageReport* report) {
  if (module.component) {
    report->status = "component";
    report->reasons.push_back(
        "the module is a component (wasm32-wasip2 shape); this build does not host the "
        "component layer yet, so it cannot be approved");
    return;
  }
  bool blocked = false;
  for (const ImportRef& import : module.imports) {
    if (import.kind != "function") {
      blocked = true;
      report->reasons.push_back("imports " + import.module + "/" + import.name + " as a " +
                                import.kind + "; a plugin must define its own");
      continue;
    }
    if (import.module != kHostNamespace) {
      blocked = true;
      report->reasons.push_back("imports " + import.module + "/" + import.name +
                                ", which is outside the pinned " + kHostNamespace + " surface");
      continue;
    }
    if (!known_host_function(import.name)) {
      blocked = true;
      report->reasons.push_back("imports " + import.module + "/" + import.name +
                                ", which the host does not provide");
    }
  }
  report->status = blocked ? "blocked" : "hostable";
}

bool parse_manifest(const std::filesystem::path& path, WasmPackage* package, std::string* error) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, kMaxManifestBytes, &bytes, error))
    return false;
  std::istringstream stream(std::string(bytes.begin(), bytes.end()));
  std::string line;
  std::size_t line_number = 0;
  std::set<std::string> seen;
  while (std::getline(stream, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos)
      line.erase(comment);
    const std::size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos)
      continue;
    const std::size_t last = line.find_last_not_of(" \t");
    line = line.substr(first, last - first + 1);
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": expected key = value";
      return false;
    }
    std::string key = line.substr(0, equals);
    std::string value = line.substr(equals + 1);
    const std::size_t key_end = key.find_last_not_of(" \t");
    key = key_end == std::string::npos ? "" : key.substr(0, key_end + 1);
    const std::size_t value_start = value.find_first_not_of(" \t");
    value = value_start == std::string::npos ? "" : value.substr(value_start);
    if (key.empty()) {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": the key is empty";
      return false;
    }
    if (!seen.insert(key).second) {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": duplicate key \"" +
               key + "\"";
      return false;
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
      value = value.substr(1, value.size() - 2);
    if (key == "name") {
      package->name = value;
    } else if (key == "version") {
      package->version = value;
    } else if (key == "module") {
      package->module = value;
    } else if (key == "api") {
      unsigned parsed = 0;
      if (value.empty() || value.size() > 3) {
        *error = path.generic_string() + ":" + std::to_string(line_number) + ": api must be 1";
        return false;
      }
      for (const char c : value) {
        if (c < '0' || c > '9') {
          *error = path.generic_string() + ":" + std::to_string(line_number) + ": api must be 1";
          return false;
        }
        parsed = parsed * 10 + static_cast<unsigned>(c - '0');
      }
      package->api = parsed;
    }
  }
  for (const char* required : {"name", "version", "module"}) {
    if (seen.count(required) == 0) {
      *error = path.generic_string() + ": missing required key \"" + required + "\"";
      return false;
    }
  }
  if (seen.count("api") == 0) {
    *error = path.generic_string() + ": missing required key \"api\"";
    return false;
  }
  if (package->api != 1) {
    *error = path.generic_string() + ": api " + std::to_string(package->api) +
             " is not supported (only 1)";
    return false;
  }
  if (package->name.empty()) {
    *error = path.generic_string() + ": the plugin name is empty";
    return false;
  }
  if (package->version.empty()) {
    *error = path.generic_string() + ": the plugin version is empty";
    return false;
  }
  return true;
}

bool safe_module_path(const std::string& text, std::filesystem::path* out) {
  if (text.empty() || text.size() > 255)
    return false;
  const std::filesystem::path path(text);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
    return false;
  for (const auto& part : path) {
    if (part == ".." || part == ".")
      return false;
  }
  const std::string extension = path.extension().string();
  if (extension != ".wasm")
    return false;
  *out = path;
  return true;
}

} // namespace

ModuleReport inspect_module(const std::filesystem::path& path) {
  ModuleReport report;
  std::vector<std::uint8_t> bytes;
  std::string error;
  if (!read_file(path, kMaxModuleBytes, &bytes, &error)) {
    report.error = error;
    return report;
  }
  report.module.file = path.generic_string();
  report.module.size = bytes.size();
  report.module.sha256 = sha256_hex(bytes.data(), bytes.size());
  if (bytes.size() < 8 || bytes[0] != 0x00 || bytes[1] != 0x61 || bytes[2] != 0x73 ||
      bytes[3] != 0x6d) {
    report.error = path.generic_string() + " is not a WebAssembly binary (bad magic)";
    return report;
  }
  const bool core = bytes[4] == 0x01 && bytes[5] == 0x00 && bytes[6] == 0x00 && bytes[7] == 0x00;
  const bool component =
      bytes[4] == 0x0d && bytes[5] == 0x00 && bytes[6] == 0x01 && bytes[7] == 0x00;
  if (!core && !component) {
    char version[96];
    std::snprintf(version, sizeof(version), "%02x %02x %02x %02x", bytes[4], bytes[5], bytes[6],
                  bytes[7]);
    report.error = path.generic_string() + " uses unsupported WebAssembly version bytes " + version;
    return report;
  }
  report.module.component = component;
  report.module.version = 1;
  const bool ok = component ? parse_component(bytes.data(), bytes.size(), &report.module, &error)
                            : parse_core_module(bytes.data(), bytes.size(), &report.module, &error);
  if (!ok) {
    report.error = path.generic_string() + ": " + error;
    report.module.imports.clear();
    report.module.exports.clear();
    return report;
  }
  report.ok = true;
  return report;
}

PackageReport inspect_wasm(const std::filesystem::path& path) {
  PackageReport report;
  std::error_code status_error;
  const bool directory = std::filesystem::is_directory(path, status_error);
  std::filesystem::path module_path = path;

  if (directory) {
    std::error_code absolute_error;
    report.package.dir = std::filesystem::absolute(path, absolute_error).lexically_normal();
    if (absolute_error)
      report.package.dir = path.lexically_normal();
    std::string error;
    if (!parse_manifest(path / "+mc-wasm.toml", &report.package, &error)) {
      report.status = "unreadable";
      report.error = error;
      return report;
    }
    std::filesystem::path relative;
    if (!safe_module_path(report.package.module, &relative)) {
      report.status = "unreadable";
      report.error = "+mc-wasm.toml: module must be a relative .wasm path inside the package";
      return report;
    }
    module_path = path / relative;
  } else if (status_error) {
    report.status = "unreadable";
    report.error = "cannot inspect " + path.generic_string() + ": " + status_error.message();
    return report;
  }

  const ModuleReport module = inspect_module(module_path);
  report.module = module.module;
  if (!module.ok) {
    report.status = "unreadable";
    report.error = module.error;
    return report;
  }
  classify(report.module, &report);
  report.ok = true;
  return report;
}

std::string package_text(const PackageReport& report) {
  std::string out = "+MC wasm plugin\n";
  if (!report.package.dir.empty()) {
    out += "package: " + report.package.dir.string() + " (" + report.package.name + " " +
           report.package.version + ", api " + std::to_string(report.package.api) + ")\n";
  }
  if (!report.module.file.empty()) {
    out += "module: " + report.module.file + "  " + std::to_string(report.module.size) +
           " bytes  sha256 " + report.module.sha256 + "\n";
  }
  if (!report.ok)
    return out + "status: unreadable\nerror: " + report.error + "\n";

  out += std::string("kind: ") + (report.module.component ? "component" : "core module") + "\n";
  out += "types: " + std::to_string(report.module.type_count) +
         "  functions: " + std::to_string(report.module.function_count) +
         "  memories: " + std::to_string(report.module.memory_count) +
         "  tables: " + std::to_string(report.module.table_count) +
         "  globals: " + std::to_string(report.module.global_count) + "\n";
  out += "imports: " + std::to_string(report.module.imports.size()) +
         "  exports: " + std::to_string(report.module.exports.size()) + "\n";
  out += "status: " + report.status + "\n";
  for (const std::string& reason : report.reasons)
    out += "  - " + reason + "\n";

  if (!report.module.imports.empty()) {
    out += "imports:\n";
    const std::size_t shown = std::min(report.module.imports.size(), kMaxDisplayedEntries);
    for (std::size_t i = 0; i < shown; ++i) {
      const ImportRef& import = report.module.imports[i];
      out += "  " + import.module + "/" + import.name + "  " + import.kind;
      if (!import.signature.empty())
        out += " " + import.signature;
      const bool provided = import.kind == "function" && import.module == kHostNamespace &&
                            known_host_function(import.name);
      out += provided ? "  provided\n" : "  refused\n";
    }
    if (shown < report.module.imports.size())
      out += "  ... and " + std::to_string(report.module.imports.size() - shown) + " more\n";
  }
  if (!report.module.exports.empty()) {
    out += "exports:\n";
    const std::size_t shown = std::min(report.module.exports.size(), kMaxDisplayedEntries);
    for (std::size_t i = 0; i < shown; ++i) {
      const ExportRef& export_ref = report.module.exports[i];
      out += "  " + export_ref.name + "  " + export_ref.kind + " #" +
             std::to_string(export_ref.index) + "\n";
    }
    if (shown < report.module.exports.size())
      out += "  ... and " + std::to_string(report.module.exports.size() - shown) + " more\n";
  }
  return out;
}

std::string package_json(const PackageReport& report) {
  std::string out = "{\"tool\":\"plusmc-wasm\",\"schema\":1,\"mode\":\"inspect\",\"ok\":";
  out += report.ok ? "true" : "false";
  out += ",\"status\":";
  out += detail::json_quote(report.status.empty() ? "unreadable" : report.status);
  if (!report.ok)
    return out + ",\"error\":" + detail::json_quote(report.error) + "}\n";

  out += ",\"package\":{\"dir\":";
  out += detail::json_quote(report.package.dir.generic_string());
  out += ",\"name\":" + detail::json_quote(report.package.name);
  out += ",\"version\":" + detail::json_quote(report.package.version);
  out += ",\"api\":" + std::to_string(report.package.api);
  out += ",\"module\":" + detail::json_quote(report.package.module) + "}";

  out += ",\"module\":{\"file\":" + detail::json_quote(report.module.file);
  out += ",\"sha256\":" + detail::json_quote(report.module.sha256);
  out += ",\"size\":" + std::to_string(report.module.size);
  out += ",\"kind\":";
  out += detail::json_quote(report.module.component ? "component" : "core");
  out += ",\"types\":" + std::to_string(report.module.type_count);
  out += ",\"functions\":" + std::to_string(report.module.function_count);
  out += ",\"memories\":" + std::to_string(report.module.memory_count);
  out += ",\"tables\":" + std::to_string(report.module.table_count);
  out += ",\"globals\":" + std::to_string(report.module.global_count);
  out += ",\"imports\":[";
  for (std::size_t i = 0; i < report.module.imports.size(); ++i) {
    if (i != 0)
      out += ",";
    const ImportRef& import = report.module.imports[i];
    const bool provided = import.kind == "function" && import.module == kHostNamespace &&
                          known_host_function(import.name);
    out += "{\"module\":" + detail::json_quote(import.module) +
           ",\"name\":" + detail::json_quote(import.name) +
           ",\"kind\":" + detail::json_quote(import.kind) +
           ",\"signature\":" + detail::json_quote(import.signature) +
           ",\"provided\":" + (provided ? "true" : "false") + "}";
  }
  out += "],\"exports\":[";
  for (std::size_t i = 0; i < report.module.exports.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"name\":" + detail::json_quote(report.module.exports[i].name) +
           ",\"kind\":" + detail::json_quote(report.module.exports[i].kind) +
           ",\"index\":" + std::to_string(report.module.exports[i].index) + "}";
  }
  out += "],\"notes\":";
  detail::json_list(&out, report.module.notes);
  out += "},\"reasons\":";
  detail::json_list(&out, report.reasons);
  return out + "}\n";
}

} // namespace mc::wasm
