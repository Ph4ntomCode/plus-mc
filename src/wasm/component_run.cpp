// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 8, slice 3: executing the component layer.
//
// The component binary is walked with the same bounded discipline as the gate:
// every length is checked against the bytes that remain, every vector has a
// fixed ceiling, and an unknown opcode is a refusal rather than a guess.
//
// Two shapes execute:
//
//   * the minimal +MC shape: one core module, no imports, and the lifecycle
//     exported as lifts that name the module's functions directly;
//   * the linked shape: component imports, `canon lower` definitions, a
//     synthetic core instance and a core instantiation whose arguments bind the
//     module's imports to those lowerings. A lower is resolved back to the
//     component import it targets, flattened against the module's core import
//     signature, and handed to the runtime as a `CoreImportBinding`, so the
//     module's foreign import reaches the `plusmc:host` capability it named.
//
// The canonical ABI is hosted for string parameters and string/list<string>
// results, with the memory option naming the nested module's own memory. A
// general component producer
// emits a shim module for that (the lowering's memory must exist before the
// importing module is instantiated); this build links one module, so that
// producer shape is refused by name rather than half-run.

#include "wasm/component_run.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "wasm/component.h"
#include "wasm/module.h"

namespace mc::wasm {
namespace {

constexpr std::uint8_t kComponentHeader[8] = {0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00};
constexpr std::uint32_t kMaxCoreModules = 8;
constexpr std::uint32_t kMaxCanon = 4096;
constexpr std::uint32_t kMaxExports = 4096;
constexpr std::uint32_t kMaxTypes = 4096;
constexpr std::uint32_t kMaxParams = 64;

bool fail(std::string* error, std::string text) {
  *error = std::move(text);
  return false;
}

std::string hex_byte(std::uint8_t byte) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out = "0x";
  out.push_back(kDigits[byte >> 4]);
  out.push_back(kDigits[byte & 0x0f]);
  return out;
}

// A bounds-checked reader over one slice. `position()` and `remaining()` let a
// section header slice its payload before the next header is read.
class Cursor {
public:
  Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  bool byte(std::uint8_t* out) {
    if (pos_ >= size_)
      return false;
    *out = data_[pos_++];
    return true;
  }

  bool uleb(std::uint32_t* out) {
    std::uint32_t value = 0;
    int shift = 0;
    for (;;) {
      std::uint8_t part = 0;
      if (!byte(&part))
        return false;
      if (shift >= 32)
        return false;
      value |= static_cast<std::uint32_t>(part & 0x7f) << shift;
      if ((part & 0x80) == 0)
        break;
      shift += 7;
    }
    *out = value;
    return true;
  }

  bool skip(std::size_t count) {
    if (count > size_ - pos_)
      return false;
    pos_ += count;
    return true;
  }

  bool name(std::string* out) {
    std::uint32_t length = 0;
    if (!uleb(&length) || length > size_ - pos_)
      return false;
    out->assign(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return true;
  }

  bool at_end() const {
    return pos_ >= size_;
  }

  bool peek(std::uint8_t* out) const {
    if (pos_ >= size_)
      return false;
    *out = data_[pos_];
    return true;
  }

  std::size_t position() const {
    return pos_;
  }

  std::size_t remaining() const {
    return size_ - pos_;
  }

private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// A component-level value type: the primitives this host flattens, plus string,
// list<string>, list<u8>, list<u16>, the flammable record and its option, the
// rgb-color record, a two-string record, a block-pos record, UUID's two-u64
// record, Pumpkin v0.2's full `block` record, an owned/borrowed resource handle,
// `option<handle>` and `result<_, string>`.
enum class CompValue {
  I32,
  I64,
  F32,
  F64,
  String,
  ListString,
  ListU8,
  ListU16,
  Flammable,
  OptionFlammable,
  RgbColor,
  RecordStrings,
  BlockPos,
  Uuid,
  BlockRecord,
  AttributeModifier,
  ItemAttributeModifier,
  NbtEntry,
  NbtTag,
  NbtTree,
  ListNbtTag,
  ListAttributeModifier,
  EnchantmentValue,
  ListEnchantmentValue,
  CustomEnchantmentValue,
  ListCustomEnchantmentValue,
  DataComponentValue,
  ListComponentValue,
  ListHandle,
  OptionString,
  OptionBool,
  OptionI32,
  OptionNbtTree,
  Handle,
  OptionHandle,
  ResultString,
  PermissionDefault,
  PermissionChild,
  ListPermissionChild,
  PermissionRecord,
  MarketplaceMetadata,
  OptionMarketplaceMetadata,
  OptionF32,
  OptionF64,
  OptionI64,
  BoundsF32,
  BoundsF64,
  BoundsI32,
  BoundsI64,
  ArgumentType
};

struct CompSignature {
  std::vector<CompValue> params;
  bool has_result = false;
  CompValue result = CompValue::I32;
  bool async = false;
};

// A component type definition. Only function and instance types are hosted;
// an instance type records the function members its import promises.
struct CompType {
  bool instance = false;
  bool enum_type = false;
  bool resource = false;
  // A named record type: `value` is its flattened shape, so a later function
  // type can refer to `block-pos` by type index.
  bool value_type = false;
  CompValue value = CompValue::I32;
  CompSignature func;
  std::map<std::string, std::uint32_t> members;
};

struct CanonOptions {
  bool async = false;
  bool has_memory = false;
  std::uint32_t memory = 0;
  bool has_realloc = false;
  std::uint32_t realloc = 0;
  bool has_post_return = false;
  std::uint32_t post_return = 0;
};

struct CompFuncEntry {
  enum class Kind { Lift, Alias, Import } kind = Kind::Import;
  std::uint32_t type_index = 0;
  std::uint32_t core_index = 0;
  std::uint32_t instance = 0;
  std::string member;
  CanonOptions options;
};

struct CoreFuncEntry {
  // A core function is either an alias of the module's export, a canon lower,
  // or one of the two canonical built-ins the v0.2 lifecycle uses.
  enum class Kind {
    Lower,
    ResourceDrop,
    TaskReturn,
    WaitableSetNew,
    WaitableSetJoin,
    WaitableSetWait
  } kind = Kind::Lower;
  bool alias = false;
  std::uint32_t component_func = 0;
  CanonOptions options;
  std::uint32_t type_index = 0;
  CompSignature task_result;
  std::uint32_t instance = 0;
  std::string name;
};

struct CoreExportEntry {
  std::string name;
  std::uint8_t sort = 0;
  std::uint32_t index = 0;
};

struct CoreInstanceEntry {
  bool instantiate = false;
  std::uint32_t module = 0;
  std::vector<std::pair<std::string, std::uint32_t>> args;
  std::vector<CoreExportEntry> exports;
};

struct CompInstanceEntry {
  std::string name;
  std::uint32_t type_index = 0;
};

struct CompExportEntry {
  std::string name;
  std::uint8_t sort = 0;
  std::uint32_t index = 0;
  bool core = false;
  std::uint8_t core_sort = 0;
};

struct Shape {
  std::vector<std::vector<std::uint8_t>> modules;
  std::vector<CompType> types;
  std::vector<CompFuncEntry> funcs;
  std::vector<CoreFuncEntry> core_funcs;
  std::vector<std::pair<std::uint32_t, std::string>> core_memories;
  std::vector<CoreInstanceEntry> core_instances;
  std::vector<CompInstanceEntry> instances;
  std::vector<CompExportEntry> exports;
  std::uint32_t imports = 0;
  std::string first_import;
  std::uint32_t lowers = 0;
  std::uint32_t aliases = 0;
  std::uint32_t nested_components = 0;
  std::uint32_t instance_defs = 0;
  std::uint32_t starts = 0;
  std::uint32_t values = 0;

  // A component with no linking definitions: lifts name the module's functions.
  bool minimal() const {
    return aliases == 0 && core_funcs.empty() && core_instances.empty() && instances.empty();
  }
};

const char* sort_name(const CompExportEntry& entry) {
  if (entry.core)
    return "core definition";
  switch (entry.sort) {
  case 0x01:
    return "function";
  case 0x02:
    return "value";
  case 0x03:
    return "type";
  case 0x04:
    return "component";
  case 0x05:
    return "instance";
  default:
    return "unknown sort";
  }
}

bool read_extern_name(Cursor* cursor, std::string* out, std::string* error) {
  std::uint8_t attributes = 0;
  if (!cursor->byte(&attributes))
    return fail(error, "a component name is truncated");
  if (attributes == 0x02)
    return fail(error, "attribute-annotated component names are not hosted");
  if (attributes != 0x00 && attributes != 0x01)
    return fail(error,
                "a component name has an unknown attribute byte " + std::to_string(attributes));
  if (!cursor->name(out))
    return fail(error, "a component name runs past its section");
  return true;
}

bool read_value_type(Cursor* cursor, const std::vector<CompType>& types, CompValue* out,
                     std::string* error);

// Reads the bounded numeric tuple Pumpkin v0.2 uses inside `argument-type`:
// two optional bounds of the same width. The canonical-ABI join widens every
// value in the variant payload area, but the guest-side types stay distinct.
bool read_tuple_value(Cursor* cursor, const std::vector<CompType>& types, CompValue* out,
                      std::string* error) {
  std::uint32_t fields = 0;
  if (!cursor->uleb(&fields))
    return fail(error, "a tuple type has no field count");
  if (fields != 2)
    return fail(error, "a tuple type other than argument-type's two-option bounds is not hosted");
  CompValue first = CompValue::I32;
  CompValue second = CompValue::I32;
  if (!read_value_type(cursor, types, &first, error) ||
      !read_value_type(cursor, types, &second, error))
    return false;
  if (first == CompValue::OptionF32 && second == CompValue::OptionF32)
    *out = CompValue::BoundsF32;
  else if (first == CompValue::OptionF64 && second == CompValue::OptionF64)
    *out = CompValue::BoundsF64;
  else if (first == CompValue::OptionI32 && second == CompValue::OptionI32)
    *out = CompValue::BoundsI32;
  else if (first == CompValue::OptionI64 && second == CompValue::OptionI64)
    *out = CompValue::BoundsI64;
  else
    return fail(error, "a tuple type other than argument-type's two-option bounds is not hosted");
  return true;
}

// Reads a record's fields. Pumpkin v0.2 uses records for `rgb-color` (three
// u8s), `block-pos` (three s32s), for the two-string config pair this host
// returns, for `flammable` (two u8s) and for the 15-field `block` result.
// Everything else is refused before it can be mistaken for a parameter.
bool read_record_value(Cursor* cursor, const std::vector<CompType>& types, CompValue* out,
                       std::string* error) {
  std::uint32_t fields = 0;
  if (!cursor->uleb(&fields))
    return fail(error, "a record type has no field count");
  if (fields != 2 && fields != 3 && fields != 4 && fields != 10 && fields != 15)
    return fail(error, "a record type other than block-pos, flammable, a two-string pair, "
                       "permission, marketplace-metadata or block is not hosted");
  if (fields == 15) {
    static constexpr const char* kBlockFields[15] = {
        "id",
        "name",
        "hardness",
        "blast-resistance",
        "map-color",
        "slipperiness",
        "velocity-multiplier",
        "jump-velocity-multiplier",
        "item-id",
        "default-state-id",
        "state-ids",
        "is-solid",
        "is-air",
        "is-flammable",
        "flammable",
    };
    static constexpr CompValue kBlockTypes[15] = {
        CompValue::I32, CompValue::String,  CompValue::F32,
        CompValue::F32, CompValue::I32,     CompValue::F32,
        CompValue::F32, CompValue::F32,     CompValue::I32,
        CompValue::I32, CompValue::ListU16, CompValue::I32,
        CompValue::I32, CompValue::I32,     CompValue::OptionFlammable,
    };
    for (std::uint32_t field = 0; field < fields; ++field) {
      std::string label;
      if (!cursor->name(&label))
        return fail(error, "a block record field name runs past its type");
      CompValue field_type = CompValue::I32;
      if (!read_value_type(cursor, types, &field_type, error))
        return false;
      if (label != kBlockFields[field] || field_type != kBlockTypes[field])
        return fail(error, "a record named `block` does not match Pumpkin v0.2's block record");
    }
    *out = CompValue::BlockRecord;
    return true;
  }

  std::vector<std::string> names;
  std::vector<CompValue> field_types;
  names.reserve(fields);
  field_types.reserve(fields);
  for (std::uint32_t field = 0; field < fields; ++field) {
    std::string label;
    if (!cursor->name(&label))
      return fail(error, "a record field name runs past its type");
    CompValue field_type = CompValue::I32;
    if (!read_value_type(cursor, types, &field_type, error))
      return false;
    names.push_back(std::move(label));
    field_types.push_back(field_type);
  }
  if (fields == 2 && field_types[0] == CompValue::String && field_types[1] == CompValue::String) {
    *out = CompValue::RecordStrings;
    return true;
  }
  if (fields == 2 && field_types[0] == CompValue::I32 && field_types[1] == CompValue::I32 &&
      names[0] == "spread-chance" && names[1] == "burn-chance") {
    *out = CompValue::Flammable;
    return true;
  }
  if (fields == 2 && names[0] == "root" && field_types[0] == CompValue::I32 && names[1] == "tags" &&
      field_types[1] == CompValue::ListNbtTag) {
    *out = CompValue::NbtTree;
    return true;
  }
  if (fields == 2 && names[0] == "high" && field_types[0] == CompValue::I64 && names[1] == "low" &&
      field_types[1] == CompValue::I64) {
    *out = CompValue::Uuid;
    return true;
  }
  if (fields == 2 && names[0] == "key" && field_types[0] == CompValue::String &&
      names[1] == "value" && field_types[1] == CompValue::I32) {
    *out = CompValue::NbtEntry;
    return true;
  }
  if (fields == 2 && names[0] == "component" && field_types[0] == CompValue::I32 &&
      names[1] == "value" && field_types[1] == CompValue::ListU8) {
    *out = CompValue::DataComponentValue;
    return true;
  }
  if (fields == 2 && names[0] == "enchantment" && field_types[0] == CompValue::I32 &&
      names[1] == "level" && field_types[1] == CompValue::I32) {
    *out = CompValue::EnchantmentValue;
    return true;
  }
  if (fields == 2 && names[0] == "enchantment-id" && field_types[0] == CompValue::String &&
      names[1] == "level" && field_types[1] == CompValue::I32) {
    *out = CompValue::CustomEnchantmentValue;
    return true;
  }
  if (fields == 2 && names[0] == "node" && field_types[0] == CompValue::String &&
      names[1] == "value" && field_types[1] == CompValue::I32) {
    *out = CompValue::PermissionChild;
    return true;
  }
  if (fields == 4 && names[0] == "node" && field_types[0] == CompValue::String &&
      names[1] == "description" && field_types[1] == CompValue::String && names[2] == "default" &&
      field_types[2] == CompValue::PermissionDefault && names[3] == "children" &&
      field_types[3] == CompValue::ListPermissionChild) {
    *out = CompValue::PermissionRecord;
    return true;
  }
  if (fields == 10 && names[0] == "marketplace-url" && field_types[0] == CompValue::String &&
      names[1] == "plugin-id" && field_types[1] == CompValue::I64 && names[2] == "plugin-name" &&
      field_types[2] == CompValue::String && names[3] == "version" &&
      field_types[3] == CompValue::String && names[4] == "dev-id" &&
      field_types[4] == CompValue::I64 && names[5] == "dev-name" &&
      field_types[5] == CompValue::String && names[6] == "is-paid" &&
      field_types[6] == CompValue::I32 && names[7] == "user-id" &&
      field_types[7] == CompValue::I64 && names[8] == "license-key" &&
      field_types[8] == CompValue::OptionString && names[9] == "issued-at" &&
      field_types[9] == CompValue::String) {
    *out = CompValue::MarketplaceMetadata;
    return true;
  }
  if (fields == 3) {
    bool all_i32 = true;
    for (CompValue field_type : field_types)
      all_i32 = all_i32 && field_type == CompValue::I32;
    if (all_i32 && names[0] == "r" && names[1] == "g" && names[2] == "b") {
      *out = CompValue::RgbColor;
      return true;
    }
    if (all_i32) {
      *out = CompValue::BlockPos;
      return true;
    }
    if (names[0] == "id" && field_types[0] == CompValue::String && names[1] == "amount" &&
        field_types[1] == CompValue::F64 && names[2] == "operation" &&
        field_types[2] == CompValue::I32) {
      *out = CompValue::AttributeModifier;
      return true;
    }
    if (names[0] == "attribute" && field_types[0] == CompValue::I32 && names[1] == "modifier" &&
        field_types[1] == CompValue::AttributeModifier && names[2] == "slot" &&
        field_types[2] == CompValue::I32) {
      *out = CompValue::ItemAttributeModifier;
      return true;
    }
  }
  std::string detail;
  for (std::uint32_t i = 0; i < fields; ++i) {
    if (!detail.empty())
      detail += ", ";
    detail += names[i] + ":" + std::to_string(static_cast<int>(field_types[i]));
  }
  return fail(error, "a record type (" + detail +
                         ") other than block-pos, flammable, attribute-modifier, "
                         "item-attribute-modifier, enchantment-value, "
                         "custom-enchantment-value, nbt-tree, nbt-entry, "
                         "data-component-value, a uuid, a two-string pair or block is not hosted");
}

// Parses a variant's cases. Pumpkin v0.2's `permission-default` and
// `argument-type` are the variants this host gives value semantics to; every
// other variant is carried as an opaque nbt-tag discriminant.
bool read_variant_value(Cursor* cursor, const std::vector<CompType>& types, CompValue* out,
                        std::string* error) {
  std::uint32_t cases = 0;
  if (!cursor->uleb(&cases))
    return fail(error, "a variant type has no case count");
  if (cases == 0 || cases > kMaxTypes)
    return fail(error, "a variant type declares " + std::to_string(cases) + " cases");
  std::vector<std::string> labels;
  std::vector<bool> payloads;
  std::vector<CompValue> payload_types;
  for (std::uint32_t i = 0; i < cases; ++i) {
    std::string label;
    if (!cursor->name(&label))
      return fail(error, "a variant case label runs past the type section");
    std::uint8_t has_payload = 0;
    if (!cursor->byte(&has_payload))
      return fail(error, "a variant case has no payload flag");
    CompValue payload = CompValue::I32;
    if (has_payload == 0x01) {
      if (!read_value_type(cursor, types, &payload, error))
        return false;
    } else if (has_payload != 0x00) {
      return fail(error, "a variant case has an unknown payload flag");
    }
    labels.push_back(std::move(label));
    payloads.push_back(has_payload == 0x01);
    payload_types.push_back(payload);
  }
  std::uint8_t refines = 0;
  if (!cursor->byte(&refines))
    return fail(error, "a variant type has no refines encoding");
  if (refines == 0x01) {
    std::uint32_t refined = 0;
    if (!cursor->uleb(&refined) || refined >= types.size())
      return fail(error, "a variant refines an undeclared type");
  } else if (refines != 0x00) {
    return fail(error, "a variant type has an unknown refines encoding");
  }
  if (labels.size() == 3 && labels[0] == "deny" && labels[1] == "allow" && labels[2] == "op" &&
      !payloads[0] && !payloads[1] && payloads[2] && payload_types[2] == CompValue::I32) {
    *out = CompValue::PermissionDefault;
    return true;
  }
  // Pumpkin v0.2's `argument-type` is the only large, payload-bearing variant
  // the command builder accepts. Recognise the exact v0.2 case order and payload
  // shapes; a differently shaped variant stays opaque and is refused when a
  // tracked member tries to flatten it.
  static constexpr const char* kArgumentCases[] = {
      "bool",
      "float",
      "double",
      "integer",
      "long",
      "string",
      "entities",
      "entity",
      "players",
      "game-profile",
      "block-pos",
      "column-pos",
      "position3d",
      "position2d",
      "block-state",
      "block-predicate",
      "item",
      "item-predicate",
      "color",
      "component",
      "style",
      "message",
      "nbt-compound-tag",
      "nbt-tag",
      "nbt-path",
      "objective",
      "objective-criteria",
      "operation",
      "particle",
      "angle",
      "rotation",
      "scoreboard-slot",
      "score-holder",
      "swizzle",
      "team",
      "item-slot",
      "resource-location",
      "mob-effect",
      "function",
      "entity-anchor",
      "int-range",
      "float-range",
      "dimension",
      "gamemode",
      "difficulty",
      "time",
      "resource",
      "resource-or-tag",
      "resource-or-tag-key",
      "resource-key",
      "template-mirror",
      "template-rotation",
      "uuid",
  };
  if (labels.size() == std::size(kArgumentCases)) {
    bool matches = true;
    for (std::size_t i = 0; i < labels.size(); ++i)
      matches = matches && labels[i] == kArgumentCases[i];
    if (matches) {
      auto payload_is = [&payloads, &payload_types](std::size_t index, CompValue value) {
        return payloads[index] && payload_types[index] == value;
      };
      matches = !payloads[0] && payload_is(1, CompValue::BoundsF32) &&
                payload_is(2, CompValue::BoundsF64) && payload_is(3, CompValue::BoundsI32) &&
                payload_is(4, CompValue::BoundsI64) && payload_is(5, CompValue::I32);
      for (std::size_t i = 6; matches && i < 32; ++i)
        matches = !payloads[i];
      matches = matches && payload_is(32, CompValue::I32);
      for (std::size_t i = 33; matches && i < 45; ++i)
        matches = !payloads[i];
      matches = matches && payload_is(45, CompValue::OptionI32);
      for (std::size_t i = 46; matches && i < 50; ++i)
        matches = payload_is(i, CompValue::String);
      for (std::size_t i = 50; matches && i < std::size(kArgumentCases); ++i)
        matches = !payloads[i];
      if (matches) {
        *out = CompValue::ArgumentType;
        return true;
      }
    }
  }
  *out = CompValue::NbtTag;
  return true;
}

bool read_value_type(Cursor* cursor, const std::vector<CompType>& types, CompValue* out,
                     std::string* error) {
  std::uint8_t byte = 0;
  if (!cursor->byte(&byte))
    return fail(error, "a value type is truncated");
  switch (byte) {
  case 0x7f: // bool
  case 0x7e: // s8
  case 0x7d: // u8
  case 0x7c: // s16
  case 0x7b: // u16
  case 0x7a: // s32
  case 0x79: // u32
    *out = CompValue::I32;
    return true;
  case 0x78: // s64
  case 0x77: // u64
    *out = CompValue::I64;
    return true;
  case 0x76:
    *out = CompValue::F32;
    return true;
  case 0x75:
    *out = CompValue::F64;
    return true;
  case 0x73:
    *out = CompValue::String;
    return true;
  case 0x70: { // list
    std::uint8_t element_byte = 0;
    if (!cursor->peek(&element_byte))
      return fail(error, "a list element type is truncated");
    CompValue element = CompValue::I32;
    if (!read_value_type(cursor, types, &element, error))
      return false;
    if (element == CompValue::String) {
      *out = CompValue::ListString;
      return true;
    }
    if (element == CompValue::I32 && element_byte == 0x7d) {
      *out = CompValue::ListU8;
      return true;
    }
    if (element == CompValue::I32 && element_byte == 0x7b) {
      *out = CompValue::ListU16;
      return true;
    }
    if (element == CompValue::NbtTag) {
      *out = CompValue::ListNbtTag;
      return true;
    }
    if (element == CompValue::ItemAttributeModifier) {
      *out = CompValue::ListAttributeModifier;
      return true;
    }
    if (element == CompValue::DataComponentValue) {
      *out = CompValue::ListComponentValue;
      return true;
    }
    if (element == CompValue::EnchantmentValue) {
      *out = CompValue::ListEnchantmentValue;
      return true;
    }
    if (element == CompValue::CustomEnchantmentValue) {
      *out = CompValue::ListCustomEnchantmentValue;
      return true;
    }
    if (element == CompValue::PermissionChild) {
      *out = CompValue::ListPermissionChild;
      return true;
    }
    if (element == CompValue::Handle) {
      *out = CompValue::ListHandle;
      return true;
    }
    return fail(error, "a list element type other than string, u8, u16, nbt-tag, "
                       "item-attribute-modifier, enchantment-value, "
                       "custom-enchantment-value, data-component-value or a resource "
                       "is not hosted");
  }
  case 0x72: // record
    return read_record_value(cursor, types, out, error);
  case 0x71: // variant
    return read_variant_value(cursor, types, out, error);
  case 0x6f: // tuple
    return read_tuple_value(cursor, types, out, error);
  case 0x6b: { // option<T>
    std::uint8_t payload_byte = 0;
    if (!cursor->peek(&payload_byte))
      return fail(error, "an option payload type is truncated");
    CompValue payload = CompValue::I32;
    if (!read_value_type(cursor, types, &payload, error))
      return false;
    if (payload == CompValue::Flammable) {
      *out = CompValue::OptionFlammable;
      return true;
    }
    if (payload == CompValue::Handle) {
      *out = CompValue::OptionHandle;
      return true;
    }
    if (payload == CompValue::String) {
      *out = CompValue::OptionString;
      return true;
    }
    if (payload == CompValue::NbtTree) {
      *out = CompValue::OptionNbtTree;
      return true;
    }
    if (payload == CompValue::MarketplaceMetadata) {
      *out = CompValue::OptionMarketplaceMetadata;
      return true;
    }
    if (payload == CompValue::I32 && payload_byte == 0x7f) {
      *out = CompValue::OptionBool;
      return true;
    }
    if (payload == CompValue::F32) {
      *out = CompValue::OptionF32;
      return true;
    }
    if (payload == CompValue::F64) {
      *out = CompValue::OptionF64;
      return true;
    }
    if (payload == CompValue::I64) {
      *out = CompValue::OptionI64;
      return true;
    }
    if (payload == CompValue::I32) {
      *out = CompValue::OptionI32;
      return true;
    }
    return fail(error, "an option type other than option<flammable>, option<handle>, "
                       "option<string>, option<bool>, option<f32>, option<f64>, "
                       "option<s64>, option<u32> or option<nbt-tree> is not hosted");
  }
  default:
    break;
  }
  if (byte == 0x69 || byte == 0x68) { // own / borrow
    std::uint32_t type_index = 0;
    if (!cursor->uleb(&type_index) || type_index >= types.size())
      return fail(error, "a resource handle names an undeclared type");
    if (!types[type_index].resource)
      return fail(error, "a resource handle names component type " + std::to_string(type_index) +
                             ", which is not a resource");
    *out = CompValue::Handle;
    return true;
  }
  if (byte == 0x6a) { // result
    std::uint8_t has_ok = 0;
    if (!cursor->byte(&has_ok))
      return fail(error, "a result type is truncated");
    if (has_ok == 0x01) {
      CompValue ignored = CompValue::I32;
      if (!read_value_type(cursor, types, &ignored, error))
        return false;
      return fail(error, "a result with an ok type is not hosted; Pumpkin v0.2 uses "
                         "result<_, string>");
    }
    if (has_ok != 0x00)
      return fail(error, "a result type has an unknown ok flag");
    std::uint8_t has_error = 0;
    if (!cursor->byte(&has_error))
      return fail(error, "a result type is truncated");
    if (has_error == 0x01) {
      CompValue error_type = CompValue::I32;
      if (!read_value_type(cursor, types, &error_type, error))
        return false;
      if (error_type != CompValue::String)
        return fail(error, "a result error type other than string is not hosted");
      *out = CompValue::ResultString;
      return true;
    }
    if (has_error != 0x00)
      return fail(error, "a result type has an unknown error flag");
    return fail(error, "a result with neither an ok nor an error type is not hosted");
  }
  if (byte < 0x40) {
    const std::uint32_t type_index = byte;
    if (type_index >= types.size())
      return fail(error,
                  "a value type names undeclared component type " + std::to_string(type_index));
    if (types[type_index].resource) {
      *out = CompValue::Handle;
      return true;
    }
    if (types[type_index].value_type) {
      *out = types[type_index].value;
      return true;
    }
    if (!types[type_index].enum_type)
      return fail(error, "a value type names component type " + std::to_string(type_index) +
                             ", which is not an enum, resource, or hosted record");
    // An enum's canonical representation is its i32 discriminant.
    *out = CompValue::I32;
    return true;
  }
  return fail(error, "value type " + hex_byte(byte) + " is not hosted");
}

bool read_label(Cursor* cursor, std::string* error) {
  std::string label;
  if (!cursor->name(&label))
    return fail(error, "a parameter or result label is truncated");
  return true;
}

// The caller has already consumed the `0x40` function-type opcode.
bool read_component_signature(Cursor* cursor, const std::vector<CompType>& types,
                              CompSignature* out, std::string* error) {
  std::uint32_t params = 0;
  if (!cursor->uleb(&params))
    return fail(error, "a component function type has no parameter count");
  if (params > kMaxParams)
    return fail(error, "a component function type declares " + std::to_string(params) +
                           " parameters, more than this host flattens");
  for (std::uint32_t i = 0; i < params; ++i) {
    if (!read_label(cursor, error))
      return false;
    CompValue value = CompValue::I32;
    if (!read_value_type(cursor, types, &value, error))
      return false;
    out->params.push_back(value);
  }
  std::uint8_t results = 0;
  if (!cursor->byte(&results))
    return fail(error, "a component function type has no result encoding");
  if (results == 0x00) {
    if (!read_value_type(cursor, types, &out->result, error))
      return false;
    out->has_result = true;
    return true;
  }
  if (results != 0x01)
    return fail(error,
                "a component function type has unknown result encoding " + std::to_string(results));
  std::uint32_t count = 0;
  if (!cursor->uleb(&count))
    return fail(error, "a component function result list is truncated");
  if (count > 1)
    return fail(error, "a component function type declares " + std::to_string(count) +
                           " results; multi-value results are not hosted");
  if (count == 1) {
    if (!read_label(cursor, error))
      return false;
    if (!read_value_type(cursor, types, &out->result, error))
      return false;
    out->has_result = true;
  }
  return true;
}

bool read_types(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the type section is truncated");
  if (count > kMaxTypes)
    return fail(error, "the type section declares " + std::to_string(count) + " types");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t kind = 0;
    if (!section->byte(&kind))
      return fail(error, "a component type is truncated");
    CompType type;
    if (kind == 0x40) {
      if (!read_component_signature(section, shape->types, &type.func, error))
        return false;
    } else if (kind == 0x43) { // async func
      if (!read_component_signature(section, shape->types, &type.func, error))
        return false;
      type.func.async = true;
    } else if (kind == 0x3f) { // resource(rep i32, dtor?)
      type.resource = true;
      std::uint8_t representation = 0;
      std::uint8_t destructor = 0;
      if (!section->byte(&representation) || representation != 0x7f)
        return fail(error, "a resource type has a non-i32 representation");
      if (!section->byte(&destructor))
        return fail(error, "a resource type has no destructor flag");
      if (destructor == 0x01) {
        std::uint32_t function = 0;
        if (!section->uleb(&function))
          return fail(error, "a resource destructor is truncated");
      } else if (destructor != 0x00) {
        return fail(error, "a resource type has an unknown destructor flag");
      }
    } else if (kind == 0x72) { // record
      type.value_type = true;
      if (!read_record_value(section, shape->types, &type.value, error))
        return false;
    } else if (kind == 0x71) { // variant
      type.value_type = true;
      if (!read_variant_value(section, shape->types, &type.value, error))
        return false;
    } else if (kind == 0x70) { // named list type
      std::uint8_t element_byte = 0;
      if (!section->peek(&element_byte))
        return fail(error, "a named list element type is truncated");
      CompValue element = CompValue::I32;
      if (!read_value_type(section, shape->types, &element, error))
        return false;
      type.value_type = true;
      if (element == CompValue::String)
        type.value = CompValue::ListString;
      else if (element == CompValue::I32 && element_byte == 0x7d)
        type.value = CompValue::ListU8;
      else if (element == CompValue::I32 && element_byte == 0x7b)
        type.value = CompValue::ListU16;
      else if (element == CompValue::NbtTag)
        type.value = CompValue::ListNbtTag;
      else if (element == CompValue::ItemAttributeModifier)
        type.value = CompValue::ListAttributeModifier;
      else if (element == CompValue::DataComponentValue)
        type.value = CompValue::ListComponentValue;
      else if (element == CompValue::EnchantmentValue)
        type.value = CompValue::ListEnchantmentValue;
      else if (element == CompValue::CustomEnchantmentValue)
        type.value = CompValue::ListCustomEnchantmentValue;
      else if (element == CompValue::PermissionChild)
        type.value = CompValue::ListPermissionChild;
      else if (element == CompValue::Handle)
        type.value = CompValue::ListHandle;
      else
        return fail(error, "a named list element type is not hosted");
    } else if (kind == 0x6b) { // named option type
      std::uint8_t payload_byte = 0;
      if (!section->peek(&payload_byte))
        return fail(error, "a named option payload type is truncated");
      CompValue payload = CompValue::I32;
      if (!read_value_type(section, shape->types, &payload, error))
        return false;
      type.value_type = true;
      if (payload == CompValue::Flammable)
        type.value = CompValue::OptionFlammable;
      else if (payload == CompValue::Handle)
        type.value = CompValue::OptionHandle;
      else if (payload == CompValue::String)
        type.value = CompValue::OptionString;
      else if (payload == CompValue::NbtTree)
        type.value = CompValue::OptionNbtTree;
      else if (payload == CompValue::I32 && payload_byte == 0x7f)
        type.value = CompValue::OptionBool;
      else if (payload == CompValue::F32)
        type.value = CompValue::OptionF32;
      else if (payload == CompValue::F64)
        type.value = CompValue::OptionF64;
      else if (payload == CompValue::I64)
        type.value = CompValue::OptionI64;
      else if (payload == CompValue::MarketplaceMetadata)
        type.value = CompValue::OptionMarketplaceMetadata;
      else if (payload == CompValue::I32)
        type.value = CompValue::OptionI32;
      else
        return fail(error, "a named option payload type is not hosted");
    } else if (kind == 0x68 || kind == 0x69) { // borrow / own resource
      type.value_type = true;
      type.value = CompValue::Handle;
      std::uint32_t resource = 0;
      if (!section->uleb(&resource) || resource >= shape->types.size())
        return fail(error, "a resource handle names an undeclared type");
      if (!shape->types[resource].resource)
        return fail(error, "a resource handle names a non-resource type");
    } else if (kind == 0x6d) {
      type.enum_type = true;
      std::uint32_t variants = 0;
      if (!section->uleb(&variants))
        return fail(error, "an enum type is truncated");
      if (variants == 0 || variants > kMaxTypes)
        return fail(error, "an enum type declares " + std::to_string(variants) + " variants");
      for (std::uint32_t v = 0; v < variants; ++v) {
        std::string label;
        if (!section->name(&label))
          return fail(error, "an enum variant label runs past the type section");
      }
    } else if (kind == 0x42) {
      type.instance = true;
      std::uint32_t declarations = 0;
      if (!section->uleb(&declarations))
        return fail(error, "an instance type is truncated");
      for (std::uint32_t d = 0; d < declarations; ++d) {
        std::uint8_t declaration = 0;
        if (!section->byte(&declaration))
          return fail(error, "an instance type declaration is truncated");
        if (declaration != 0x04) {
          return fail(error, "an instance type declaration of kind " + std::to_string(declaration) +
                                 " is not hosted (only function exports are)");
        }
        std::string name;
        if (!read_extern_name(section, &name, error))
          return false;
        std::uint8_t sort = 0;
        if (!section->byte(&sort))
          return fail(error, "instance export " + name + " has no sort");
        if (sort != 0x01) {
          return fail(error, "instance export " + name +
                                 " is not a function; only function exports are hosted");
        }
        std::uint32_t type_index = 0;
        if (!section->uleb(&type_index) || type_index >= shape->types.size())
          return fail(error, "instance export " + name + " names an undeclared type");
        if (shape->types[type_index].instance || shape->types[type_index].resource ||
            shape->types[type_index].enum_type || shape->types[type_index].value_type)
          return fail(error, "instance export " + name + " is not a function type");
        if (!type.members.emplace(name, type_index).second)
          return fail(error, "instance type exports " + name + " more than once");
      }
    } else {
      return fail(error, "a component type of kind " + hex_byte(kind) +
                             " is not hosted (only function and instance types are)");
    }
    shape->types.push_back(std::move(type));
  }
  return true;
}

bool read_imports(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the import section is truncated");
  shape->imports += count;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string name;
    if (!read_extern_name(section, &name, error))
      return false;
    if (shape->first_import.empty())
      shape->first_import = name;
    std::uint8_t sort = 0;
    if (!section->byte(&sort))
      return fail(error, "import " + name + " has no sort");
    if (sort == 0x01) {
      std::uint32_t type_index = 0;
      if (!section->uleb(&type_index) || type_index >= shape->types.size())
        return fail(error, "import " + name + " names an undeclared function type");
      if (shape->types[type_index].instance || shape->types[type_index].resource ||
          shape->types[type_index].enum_type || shape->types[type_index].value_type)
        return fail(error, "import " + name + " names component type " +
                               std::to_string(type_index) + ", which is not a function type");
      CompFuncEntry entry;
      entry.kind = CompFuncEntry::Kind::Import;
      entry.type_index = type_index;
      shape->funcs.push_back(std::move(entry));
    } else if (sort == 0x05) {
      std::uint32_t type_index = 0;
      if (!section->uleb(&type_index) || type_index >= shape->types.size())
        return fail(error, "import " + name + " names an undeclared instance type");
      if (!shape->types[type_index].instance)
        return fail(error, "import " + name + " is an instance of a function type");
      shape->instances.push_back({name, type_index});
    } else {
      return fail(error, "import " + name + " is a component item of sort " + std::to_string(sort) +
                             ", not a function or instance");
    }
  }
  return true;
}

bool read_aliases(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the alias section is truncated");
  shape->aliases += count;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t sort = 0;
    if (!section->byte(&sort))
      return fail(error, "an alias is truncated");
    std::uint8_t target = 0;
    if (sort == 0x00) {
      std::uint8_t core_sort = 0;
      if (!section->byte(&core_sort) || !section->byte(&target))
        return fail(error, "a core alias is truncated");
      if (target != 0x01)
        return fail(error, "a core alias of target kind " + std::to_string(target) +
                               " is not hosted (outer aliases are not)");
      std::uint32_t instance = 0;
      std::string name;
      if (!section->uleb(&instance) || !section->name(&name))
        return fail(error, "a core alias is truncated");
      if (instance >= shape->core_instances.size())
        return fail(error, "a core alias names core instance " + std::to_string(instance) +
                               ", which is not defined");
      if (core_sort == 0x00) {
        CoreFuncEntry entry;
        entry.alias = true;
        entry.instance = instance;
        entry.name = std::move(name);
        shape->core_funcs.push_back(std::move(entry));
      } else if (core_sort == 0x02) {
        shape->core_memories.emplace_back(instance, std::move(name));
      } else {
        return fail(error, "a core alias of sort " + std::to_string(core_sort) +
                               " is not hosted (only functions and memories are)");
      }
    } else if (sort == 0x01) {
      if (!section->byte(&target))
        return fail(error, "a function alias is truncated");
      if (target != 0x00)
        return fail(error, "a function alias of target kind " + std::to_string(target) +
                               " is not hosted (outer aliases are not)");
      std::uint32_t instance = 0;
      std::string member;
      if (!section->uleb(&instance) || !section->name(&member))
        return fail(error, "a function alias is truncated");
      if (instance >= shape->instances.size())
        return fail(error, "a function alias names instance " + std::to_string(instance) +
                               ", which is not defined");
      const CompInstanceEntry& source = shape->instances[instance];
      const CompType& type = shape->types[source.type_index];
      const auto found = type.members.find(member);
      if (found == type.members.end())
        return fail(error, "aliases " + member + ", which " + source.name + " does not export");
      if (found->second >= shape->types.size())
        return fail(error, "aliases " + member + ", whose type is not declared");
      const CompType& member_type = shape->types[found->second];
      if (member_type.instance || member_type.resource || member_type.enum_type ||
          member_type.value_type)
        return fail(error,
                    "aliases " + member + " from " + source.name + ", which is not a function");
      CompFuncEntry entry;
      entry.kind = CompFuncEntry::Kind::Alias;
      entry.instance = instance;
      entry.member = member;
      entry.type_index = found->second;
      shape->funcs.push_back(std::move(entry));
    } else {
      return fail(error, "a component alias of sort " + std::to_string(sort) +
                             " is not hosted (only function aliases are)");
    }
  }
  return true;
}

bool read_canon_options(Cursor* section, CanonOptions* out, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "a canonical option list is truncated");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t option = 0;
    if (!section->byte(&option))
      return fail(error, "a canonical option is truncated");
    switch (option) {
    case 0x00: // string-encoding=utf8, also the default
      break;
    case 0x01: // utf16
    case 0x02: // latin1+utf16
      return fail(error, "canonical string encoding " + std::to_string(option) +
                             " is not hosted (only utf8)");
    case 0x03: {
      std::uint32_t memory = 0;
      if (!section->uleb(&memory))
        return fail(error, "a memory canonical option is truncated");
      out->has_memory = true;
      out->memory = memory;
      break;
    }
    case 0x04: {
      std::uint32_t realloc = 0;
      if (!section->uleb(&realloc))
        return fail(error, "a realloc canonical option is truncated");
      out->has_realloc = true;
      out->realloc = realloc;
      break;
    }
    case 0x05: {
      std::uint32_t post_return = 0;
      if (!section->uleb(&post_return))
        return fail(error, "a post-return canonical option is truncated");
      out->has_post_return = true;
      out->post_return = post_return;
      break;
    }
    case 0x06:
      out->async = true;
      break;
    case 0x07:
      return fail(error, "the callback canonical option is not hosted");
    default:
      return fail(error, "canonical option " + hex_byte(option) + " is not hosted");
    }
  }
  return true;
}

bool read_canon(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the canon section is truncated");
  if (count > kMaxCanon)
    return fail(error, "the canon section declares " + std::to_string(count) + " entries");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t kind = 0;
    if (!section->byte(&kind))
      return fail(error, "a canon entry is truncated");
    if (kind == 0x00 || kind == 0x01) {
      std::uint8_t reserved = 0;
      if (!section->byte(&reserved))
        return fail(error, "a canon entry is truncated");
      if (reserved != 0x00)
        return fail(error, "a canon entry has an unknown sort byte " + std::to_string(reserved));
    }
    if (kind == 0x00) {
      std::uint32_t core_function = 0;
      std::uint32_t type_index = 0;
      CanonOptions options;
      if (!section->uleb(&core_function) || !read_canon_options(section, &options, error) ||
          !section->uleb(&type_index)) {
        return fail(error, error->empty() ? "a canon lift is truncated" : *error);
      }
      if (type_index >= shape->types.size())
        return fail(error, "a canon lift names an undeclared type");
      if (shape->types[type_index].func.async != options.async)
        return fail(error, "the async option on a lift requires an async function type");
      CompFuncEntry entry;
      entry.kind = CompFuncEntry::Kind::Lift;
      entry.core_index = core_function;
      entry.type_index = type_index;
      entry.options = options;
      shape->funcs.push_back(std::move(entry));
    } else if (kind == 0x01) {
      std::uint32_t function = 0;
      if (!section->uleb(&function))
        return fail(error, "a canon lower is truncated");
      CanonOptions options;
      if (!read_canon_options(section, &options, error))
        return false;
      if (function >= shape->funcs.size())
        return fail(error, "canon lower names component function " + std::to_string(function) +
                               ", which is not defined");
      if (options.async) {
        const CompFuncEntry& callee = shape->funcs[function];
        if (callee.type_index >= shape->types.size())
          return fail(error, "an async canon lower names a function with no type");
        const CompSignature& callee_type = shape->types[callee.type_index].func;
        if (!callee_type.async)
          return fail(error, "the async option on a canon lower requires an async function type");
        if (callee_type.has_result)
          return fail(error, "an async canon lower with a result is not hosted; the async "
                             "Pumpkin members return no value");
        if (options.has_realloc || options.has_post_return)
          return fail(error, "an async canon lower cannot carry realloc or post-return");
      }
      if (options.has_realloc) {
        // A lower of a string result needs the module's allocator. Check the
        // callee here so a realloc on any other lower is refused rather than
        // silently ignored.
        const CompFuncEntry& callee = shape->funcs[function];
        if (callee.type_index >= shape->types.size())
          return fail(error, "a canon lower with realloc names a function with no type");
        const CompSignature& callee_type = shape->types[callee.type_index].func;
        if (!callee_type.has_result ||
            (callee_type.result != CompValue::String &&
             callee_type.result != CompValue::ListString &&
             callee_type.result != CompValue::RecordStrings &&
             callee_type.result != CompValue::BlockRecord &&
             callee_type.result != CompValue::OptionString &&
             callee_type.result != CompValue::OptionNbtTree &&
             callee_type.result != CompValue::ListNbtTag &&
             callee_type.result != CompValue::ListAttributeModifier &&
             callee_type.result != CompValue::ListEnchantmentValue &&
             callee_type.result != CompValue::ListCustomEnchantmentValue &&
             callee_type.result != CompValue::ListHandle &&
             callee_type.result != CompValue::ListComponentValue &&
             callee_type.result != CompValue::ResultString &&
             callee_type.result != CompValue::OptionMarketplaceMetadata))
          return fail(error, "a canon lower carries a realloc option but its function "
                             "does not return an allocating string, list or record result");
      }
      if (options.has_post_return)
        return fail(error, "a canon lower carries a post-return option; that belongs on a lift");
      CoreFuncEntry entry;
      entry.component_func = function;
      entry.options = options;
      shape->core_funcs.push_back(std::move(entry));
      ++shape->lowers;
    } else if (kind == 0x03) { // resource.drop
      std::uint32_t type_index = 0;
      if (!section->uleb(&type_index) || type_index >= shape->types.size() ||
          !shape->types[type_index].resource) {
        return fail(error, "canon resource.drop names an undeclared resource type");
      }
      CoreFuncEntry entry;
      entry.kind = CoreFuncEntry::Kind::ResourceDrop;
      entry.type_index = type_index;
      shape->core_funcs.push_back(std::move(entry));
    } else if (kind == 0x1f) { // waitable-set.new
      CoreFuncEntry entry;
      entry.kind = CoreFuncEntry::Kind::WaitableSetNew;
      shape->core_funcs.push_back(std::move(entry));
    } else if (kind == 0x20) { // waitable-set.wait
      std::uint8_t reserved = 0;
      std::uint32_t memory = 0;
      if (!section->byte(&reserved) || reserved != 0x00 || !section->uleb(&memory))
        return fail(error, "waitable-set.wait is truncated");
      CoreFuncEntry entry;
      entry.kind = CoreFuncEntry::Kind::WaitableSetWait;
      entry.options.has_memory = true;
      entry.options.memory = memory;
      shape->core_funcs.push_back(std::move(entry));
    } else if (kind == 0x23) { // waitable-set.join
      CoreFuncEntry entry;
      entry.kind = CoreFuncEntry::Kind::WaitableSetJoin;
      shape->core_funcs.push_back(std::move(entry));
    } else if (kind == 0x09) { // task.return
      std::uint8_t has_result = 0;
      if (!section->byte(&has_result))
        return fail(error, "task.return has no result encoding");
      CompSignature result;
      if (has_result == 0x00) {
        if (!read_value_type(section, shape->types, &result.result, error))
          return false;
        result.has_result = true;
      } else if (has_result == 0x01) {
        std::uint8_t none = 0;
        if (!section->byte(&none) || none != 0x00)
          return fail(error, "task.return's empty result is malformed");
      } else {
        return fail(error, "task.return has an unknown result encoding");
      }
      CanonOptions options;
      if (!read_canon_options(section, &options, error))
        return false;
      if (result.has_result && result.result != CompValue::ResultString)
        return fail(error, "task.return must carry result<_, string> or no result");
      CoreFuncEntry entry;
      entry.kind = CoreFuncEntry::Kind::TaskReturn;
      entry.task_result = result;
      entry.options = options;
      shape->core_funcs.push_back(std::move(entry));
    } else {
      return fail(error, "canon opcode " + std::to_string(kind) +
                             " is not hosted (lift, lower, resource.drop and task.return are)");
    }
  }
  return true;
}

bool read_core_instances(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the core instance section is truncated");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint8_t kind = 0;
    if (!section->byte(&kind))
      return fail(error, "a core instance is truncated");
    CoreInstanceEntry entry;
    if (kind == 0x00) {
      entry.instantiate = true;
      if (!section->uleb(&entry.module))
        return fail(error, "a core instantiation is truncated");
      std::uint32_t args = 0;
      if (!section->uleb(&args))
        return fail(error, "a core instantiation has no argument list");
      for (std::uint32_t a = 0; a < args; ++a) {
        std::string name;
        std::uint8_t sort = 0;
        std::uint32_t instance = 0;
        if (!section->name(&name) || !section->byte(&sort) || sort != 0x12 ||
            !section->uleb(&instance)) {
          return fail(error, "a core instantiation argument is not an instance");
        }
        if (instance >= shape->core_instances.size())
          return fail(error, "a core instantiation passes instance " + std::to_string(instance) +
                                 ", which is not defined");
        entry.args.emplace_back(std::move(name), instance);
      }
    } else if (kind == 0x01) {
      std::uint32_t exports = 0;
      if (!section->uleb(&exports))
        return fail(error, "a synthetic core instance has no export list");
      for (std::uint32_t e = 0; e < exports; ++e) {
        CoreExportEntry export_entry;
        if (!section->name(&export_entry.name) || !section->byte(&export_entry.sort) ||
            !section->uleb(&export_entry.index)) {
          return fail(error, "a synthetic core export is truncated");
        }
        entry.exports.push_back(std::move(export_entry));
      }
    } else {
      return fail(error, "core instance kind " + std::to_string(kind) + " is not hosted");
    }
    shape->core_instances.push_back(std::move(entry));
  }
  return true;
}

bool read_exports(Cursor* section, Shape* shape, std::string* error) {
  std::uint32_t count = 0;
  if (!section->uleb(&count))
    return fail(error, "the export section is truncated");
  if (count > kMaxExports)
    return fail(error, "the export section declares " + std::to_string(count) + " entries");
  for (std::uint32_t i = 0; i < count; ++i) {
    CompExportEntry entry;
    if (!read_extern_name(section, &entry.name, error))
      return false;
    std::uint8_t sort = 0;
    if (!section->byte(&sort))
      return fail(error, "export " + entry.name + " has no sort");
    if (sort == 0x00) {
      entry.core = true;
      if (!section->byte(&entry.core_sort))
        return fail(error, "export " + entry.name + " has no core sort");
    } else if (sort > 0x05) {
      return fail(error,
                  "export " + entry.name + " has an unknown sort byte " + std::to_string(sort));
    }
    entry.sort = sort;
    if (!section->uleb(&entry.index))
      return fail(error, "export " + entry.name + " has no index");
    std::uint8_t ascribed = 0;
    if (!section->byte(&ascribed))
      return fail(error, "export " + entry.name + " has no ascribed-type flag");
    if (ascribed == 0x01) {
      std::uint8_t kind = 0;
      if (!section->byte(&kind))
        return fail(error, "export " + entry.name + " has a truncated ascribed type");
      if (kind == 0x02 || kind == 0x03)
        return fail(error, "export " + entry.name +
                               " ascribes a value or type bound, which is not hosted");
      std::uint32_t index = 0;
      if (!section->uleb(&index))
        return fail(error, "export " + entry.name + " has a truncated ascribed type");
    } else if (ascribed != 0x00) {
      return fail(error, "export " + entry.name + " has an unknown ascribed-type flag");
    }
    shape->exports.push_back(std::move(entry));
  }
  return true;
}

bool core_module_slice(const std::uint8_t* payload, std::size_t size, const std::uint8_t** module,
                       std::size_t* length);

bool read_shape(const std::vector<std::uint8_t>& bytes, Shape* shape, std::string* error) {
  if (bytes.size() < sizeof(kComponentHeader) ||
      std::memcmp(bytes.data(), kComponentHeader, sizeof(kComponentHeader)) != 0) {
    return fail(error, "the file is not a component (wrong header)");
  }
  const std::size_t header_size = sizeof(kComponentHeader);
  Cursor cursor(bytes.data() + header_size, bytes.size() - header_size);
  while (!cursor.at_end()) {
    std::uint8_t id = 0;
    std::uint32_t size = 0;
    if (!cursor.byte(&id) || !cursor.uleb(&size))
      return fail(error, "a component section header is truncated");
    if (size > cursor.remaining())
      return fail(error, "component section " + std::to_string(id) + " runs past the file");
    const std::uint8_t* payload = bytes.data() + header_size + cursor.position();
    if (!cursor.skip(size))
      return fail(error, "component section " + std::to_string(id) + " runs past the file");
    Cursor section(payload, size);
    switch (id) {
    case 0: // custom
      break;
    case 1: { // core module
      if (shape->modules.size() >= kMaxCoreModules)
        return fail(error, "the component holds more than " + std::to_string(kMaxCoreModules) +
                               " core modules");
      const std::uint8_t* module = nullptr;
      std::size_t length = 0;
      if (!core_module_slice(payload, size, &module, &length))
        return fail(error, "a core module section has no wasm header");
      shape->modules.emplace_back(module, module + length);
      break;
    }
    case 2:
      if (!read_core_instances(&section, shape, error))
        return false;
      break;
    case 3: // core types: no component functions
      break;
    case 4: { // nested components
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested component section is truncated");
      shape->nested_components += count;
      break;
    }
    case 5: { // component instance definitions
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the instance section is truncated");
      shape->instance_defs += count;
      break;
    }
    case 6:
      if (!read_aliases(&section, shape, error))
        return false;
      break;
    case 7:
      if (!read_types(&section, shape, error))
        return false;
      break;
    case 8:
      if (!read_canon(&section, shape, error))
        return false;
      break;
    case 9: {
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the start section is truncated");
      shape->starts += count;
      break;
    }
    case 10:
      if (!read_imports(&section, shape, error))
        return false;
      break;
    case 11:
      if (!read_exports(&section, shape, error))
        return false;
      break;
    case 12: {
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the value section is truncated");
      shape->values += count;
      break;
    }
    default:
      return fail(error, "unknown component section " + std::to_string(id));
    }
  }
  return true;
}

bool read_file(const std::string& path, std::vector<std::uint8_t>* out, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return fail(error, "cannot read " + path);
  file.seekg(0, std::ios::end);
  const std::streamoff size = file.tellg();
  file.seekg(0, std::ios::beg);
  if (size < 0)
    return fail(error, "cannot read " + path);
  out->resize(static_cast<std::size_t>(size));
  if (!out->empty())
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(out->size()));
  if (!file && !out->empty())
    return fail(error, "cannot read " + path);
  return true;
}

// The nested core module's imports and function exports, read with the same
// bounded discipline as the component. Only MVP function imports are hosted.
struct CoreSignature {
  std::vector<std::uint8_t> params;
  bool has_result = false;
  std::uint8_t result = 0x7f;
};

struct CoreImportInfo {
  std::string module;
  std::string name;
  CoreSignature signature;
};

struct CoreModuleInfo {
  std::vector<CoreImportInfo> imports;
  std::vector<CoreSignature> types;
  std::vector<std::uint32_t> defined_types;
  std::map<std::string, std::uint32_t> functions;
  bool has_memory = false;
  std::uint32_t defined_functions = 0;

  std::uint32_t function_count() const {
    return static_cast<std::uint32_t>(imports.size()) + defined_functions;
  }

  // The declared signature of a function in the module's index space, imports
  // first. The linker uses this to check that a lift names a core function with
  // the canonical shape it is about to call.
  bool signature_of(std::uint32_t index, CoreSignature* out) const {
    if (index < imports.size()) {
      *out = imports[index].signature;
      return true;
    }
    const std::uint32_t defined = index - static_cast<std::uint32_t>(imports.size());
    if (defined >= defined_types.size())
      return false;
    if (defined_types[defined] >= types.size())
      return false;
    *out = types[defined_types[defined]];
    return true;
  }
};

const char* core_value_name(std::uint8_t byte) {
  switch (byte) {
  case 0x7f:
    return "i32";
  case 0x7e:
    return "i64";
  case 0x7d:
    return "f32";
  case 0x7c:
    return "f64";
  default:
    return "?";
  }
}

std::string core_signature_text(const CoreSignature& signature) {
  std::string out = "(";
  for (std::size_t i = 0; i < signature.params.size(); ++i) {
    if (i != 0)
      out += ",";
    out += core_value_name(signature.params[i]);
  }
  out += ")->(";
  if (signature.has_result)
    out += core_value_name(signature.result);
  return out + ")";
}

bool read_core_signature(Cursor* cursor, CoreSignature* out, std::string* error) {
  std::uint8_t opcode = 0;
  if (!cursor->byte(&opcode) || opcode != 0x60)
    return fail(error, "the nested module has a non-function core type");
  std::uint32_t params = 0;
  if (!cursor->uleb(&params))
    return fail(error, "a core function type has no parameter count");
  for (std::uint32_t i = 0; i < params; ++i) {
    std::uint8_t value = 0;
    if (!cursor->byte(&value))
      return fail(error, "a core function parameter is truncated");
    if (value != 0x7f && value != 0x7e && value != 0x7d && value != 0x7c)
      return fail(error, "a core function parameter is not an MVP value type");
    out->params.push_back(value);
  }
  std::uint32_t results = 0;
  if (!cursor->uleb(&results))
    return fail(error, "a core function type has no result count");
  if (results > 1)
    return fail(error, "a core function type has multiple results, which are not hosted");
  if (results == 1) {
    std::uint8_t value = 0;
    if (!cursor->byte(&value))
      return fail(error, "a core function result is truncated");
    if (value != 0x7f && value != 0x7e && value != 0x7d && value != 0x7c)
      return fail(error, "a core function result is not an MVP value type");
    out->has_result = true;
    out->result = value;
  }
  return true;
}

bool core_module_slice(const std::uint8_t* payload, std::size_t size, const std::uint8_t** module,
                       std::size_t* length) {
  *module = payload;
  *length = size;
  if (size >= 4 && std::memcmp(payload, "\0asm", 4) == 0)
    return true;
  Cursor prefix(payload, size);
  std::uint32_t declared = 0;
  if (!prefix.uleb(&declared) || declared > prefix.remaining())
    return false;
  *module = payload + prefix.position();
  *length = declared;
  return true;
}

// A compiler-produced component can carry several core modules: the plugin's
// own module plus a shim that exports `cabi_realloc`. The compiled-core path
// only needs the module that exports the lifecycle, so it scans module bytes
// without decoding the component's (much wider) type section.
struct CoreModuleCandidate {
  const std::uint8_t* bytes = nullptr;
  std::size_t size = 0;
};

struct ComponentScan {
  std::vector<CoreModuleCandidate> modules;
  std::uint32_t imports = 0;
  std::uint32_t instance_defs = 0;
  // A component built with a standard library imports WASI interfaces. That is
  // the marker this build uses to recognise a compiler-produced runner from a
  // hand-assembled fixture with the same small import count.
  bool wasi = false;
};

bool scan_component(const std::vector<std::uint8_t>& bytes, ComponentScan* out,
                    std::string* error) {
  if (bytes.size() < sizeof(kComponentHeader) ||
      std::memcmp(bytes.data(), kComponentHeader, sizeof(kComponentHeader)) != 0) {
    return fail(error, "the file is not a component (wrong header)");
  }
  static const char kWasi[] = "wasi:";
  out->wasi = std::search(bytes.begin(), bytes.end(), kWasi, kWasi + 5) != bytes.end();
  const std::size_t header_size = sizeof(kComponentHeader);
  Cursor cursor(bytes.data() + header_size, bytes.size() - header_size);
  while (!cursor.at_end()) {
    std::uint8_t id = 0;
    std::uint32_t size = 0;
    if (!cursor.byte(&id) || !cursor.uleb(&size))
      return fail(error, "a component section header is truncated");
    if (size > cursor.remaining())
      return fail(error, "component section " + std::to_string(id) + " runs past the file");
    const std::uint8_t* payload = bytes.data() + header_size + cursor.position();
    if (!cursor.skip(size))
      return fail(error, "component section " + std::to_string(id) + " runs past the file");
    if (id == 1) {
      const std::uint8_t* module = nullptr;
      std::size_t length = 0;
      if (!core_module_slice(payload, size, &module, &length))
        return fail(error, "a core module section has no wasm header");
      out->modules.push_back({module, length});
    } else if (id == 5 || id == 10) {
      Cursor section(payload, size);
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "a component count section is truncated");
      if (id == 5)
        out->instance_defs += count;
      else
        out->imports += count;
    }
  }
  return true;
}

bool read_core_module(const std::vector<std::uint8_t>& bytes, CoreModuleInfo* info,
                      std::string* error) {
  if (bytes.size() < 8 || std::memcmp(bytes.data(), "\0asm", 4) != 0)
    return fail(error, "the nested core module has no wasm header");
  std::vector<CoreSignature> types;
  Cursor cursor(bytes.data() + 8, bytes.size() - 8);
  bool memory_section = false;
  while (!cursor.at_end()) {
    std::uint8_t id = 0;
    std::uint32_t size = 0;
    if (!cursor.byte(&id) || !cursor.uleb(&size))
      return fail(error, "a nested module section header is truncated");
    if (size > cursor.remaining())
      return fail(error, "nested module section " + std::to_string(id) + " runs past the file");
    const std::uint8_t* payload = bytes.data() + 8 + cursor.position();
    if (!cursor.skip(size))
      return fail(error, "a nested module section runs past the file");
    Cursor section(payload, size);
    switch (id) {
    case 1: { // type
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested module's type section is truncated");
      for (std::uint32_t i = 0; i < count; ++i) {
        CoreSignature signature;
        if (!read_core_signature(&section, &signature, error))
          return false;
        types.push_back(std::move(signature));
      }
      break;
    }
    case 2: { // import
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested module's import section is truncated");
      for (std::uint32_t i = 0; i < count; ++i) {
        CoreImportInfo import;
        std::uint8_t kind = 0;
        std::uint32_t type_index = 0;
        if (!section.name(&import.module) || !section.name(&import.name) || !section.byte(&kind)) {
          return fail(error, "a nested module import is truncated");
        }
        if (kind != 0)
          return fail(error, "the nested module imports " + import.module + "/" + import.name +
                                 " as a non-function, which the component linker does not host");
        if (!section.uleb(&type_index) || type_index >= types.size())
          return fail(error, "the nested module imports " + import.module + "/" + import.name +
                                 " with an undeclared type");
        import.signature = types[type_index];
        info->imports.push_back(std::move(import));
      }
      break;
    }
    case 3: { // function
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested module's function section is truncated");
      info->defined_functions = count;
      for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t type_index = 0;
        if (!section.uleb(&type_index) || type_index >= types.size())
          return fail(error, "a nested module function names an undeclared type");
        info->defined_types.push_back(type_index);
      }
      break;
    }
    case 5: { // memory
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested module's memory section is truncated");
      if (count > 1)
        return fail(error, "the nested module declares multiple memories; the hosted shape has "
                           "one");
      if (count == 1) {
        std::uint8_t flags = 0;
        std::uint32_t minimum = 0;
        if (!section.byte(&flags) || !section.uleb(&minimum))
          return fail(error, "a nested module memory limit is truncated");
        if ((flags & 0x01) != 0) {
          std::uint32_t maximum = 0;
          if (!section.uleb(&maximum))
            return fail(error, "a nested module memory maximum is truncated");
        }
        memory_section = true;
      }
      break;
    }
    case 7: { // export
      std::uint32_t count = 0;
      if (!section.uleb(&count))
        return fail(error, "the nested module's export section is truncated");
      for (std::uint32_t i = 0; i < count; ++i) {
        std::string name;
        std::uint8_t kind = 0;
        std::uint32_t index = 0;
        if (!section.name(&name) || !section.byte(&kind) || !section.uleb(&index))
          return fail(error, "a nested module export is truncated");
        if (kind == 0x00)
          info->functions[name] = index;
        else if (kind == 0x02)
          memory_section = true;
      }
      break;
    }
    default:
      break;
    }
  }
  info->has_memory = memory_section;
  info->types = std::move(types);
  return true;
}

bool flatten_signature(const CompSignature& signature, CoreSignature* out, std::string* error) {
  for (const CompValue value : signature.params) {
    switch (value) {
    case CompValue::I32:
      out->params.push_back(0x7f);
      break;
    case CompValue::I64:
      out->params.push_back(0x7e);
      break;
    case CompValue::F32:
      out->params.push_back(0x7d);
      break;
    case CompValue::F64:
      out->params.push_back(0x7c);
      break;
    case CompValue::String:
      // A string is a (pointer, length) pair in the module's memory.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::ListString:
      return fail(error, "a list<string> parameter is not hosted");
    case CompValue::ListU8:
      // Pumpkin's `log-tracing(event: list<u8>)`: a byte array is the
      // canonical-ABI (pointer, length) pair into the module's memory.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::RgbColor:
      // rgb-color is a record of three u8 fields, widened to i32 by the canonical
      // ABI; the host packs them back into #rrggbb.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::OptionHandle:
      // option<text-component> is a discriminant plus the handle index.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::OptionF32:
    case CompValue::OptionF64:
    case CompValue::OptionI64:
    case CompValue::BoundsF32:
    case CompValue::BoundsF64:
    case CompValue::BoundsI32:
    case CompValue::BoundsI64:
      return fail(error, "a numeric argument-type payload is only hosted inside argument-type");
    case CompValue::ArgumentType:
      // Pumpkin v0.2's `argument-type` flattens to a discriminant plus the
      // joined payload: i32, i64, i32, i64. The compiler emits exactly this for
      // `command-node.argument`, with strings in the (i32,i64) slots.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7e);
      out->params.push_back(0x7f);
      out->params.push_back(0x7e);
      break;
    case CompValue::ListHandle:
    case CompValue::ListEnchantmentValue:
    case CompValue::ListCustomEnchantmentValue:
      // A list parameter is the canonical (pointer, length) pair.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::EnchantmentValue:
      // enchantment enum plus level.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::CustomEnchantmentValue:
      // enchantment-id (pointer, length) plus level.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::PermissionRecord:
      // A 28-byte record with strings, a variant and a child list is passed
      // indirectly as one pointer to the canonical layout.
      out->params.push_back(0x7f);
      break;
    case CompValue::PermissionDefault:
    case CompValue::PermissionChild:
    case CompValue::ListPermissionChild:
    case CompValue::MarketplaceMetadata:
    case CompValue::OptionMarketplaceMetadata:
      return fail(error, "a permission or marketplace value is not hosted as a direct parameter");
    case CompValue::ListU16:
    case CompValue::Flammable:
    case CompValue::OptionFlammable:
    case CompValue::ListNbtTag:
    case CompValue::ListAttributeModifier:
    case CompValue::ListComponentValue:
    case CompValue::NbtEntry:
    case CompValue::NbtTag:
    case CompValue::DataComponentValue:
      return fail(error, "a record/list parameter is not hosted; only block-pos is");
    case CompValue::RecordStrings:
      return fail(error, "a two-string record parameter is not hosted; only block-pos is");
    case CompValue::OptionString:
      // option<string> is a discriminant plus the string's (pointer, length).
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::OptionBool:
      // option<bool> is a discriminant and a widened flag.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::OptionNbtTree:
      // The option's nbt-tree payload is a three-word out-area.
      out->params.push_back(0x7f);
      break;
    case CompValue::OptionI32:
      // Any other option of a wrapped 32-bit value has the same two-word form.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::AttributeModifier:
      // id: string, amount: f64, operation: enum.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7c);
      out->params.push_back(0x7f);
      break;
    case CompValue::ItemAttributeModifier:
      // attribute, modifier.id (ptr/len), modifier.amount, operation, slot.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7c);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::NbtTree:
      // root plus the tags list's (pointer, length).
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::BlockRecord:
      // A record carrying a string and a list is passed indirectly: one i32
      // pointer to the canonical 56-byte block layout.
      out->params.push_back(0x7f);
      break;
    case CompValue::BlockPos:
      // Pumpkin's block-pos is a record of three s32 fields, flattened in
      // field order to the three i32 coordinates the host call takes.
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      out->params.push_back(0x7f);
      break;
    case CompValue::Uuid:
      // uuid is a record of two u64 fields, flattened in field order.
      out->params.push_back(0x7e);
      out->params.push_back(0x7e);
      break;
    case CompValue::Handle:
      // An owned or borrowed resource is a handle-table index.
      out->params.push_back(0x7f);
      break;
    case CompValue::ResultString:
      return fail(error, "a result value is carried by task.return, not a synchronous "
                         "canonical parameter");
    }
  }
  if (signature.has_result) {
    if (signature.result == CompValue::OptionHandle) {
      // option<player> is delivered through the guest's 8-byte return area. The
      // area pointer is the last canonical parameter and the core function has
      // no result; this is the shape `server.get-player-by-name` lowers to.
      out->params.push_back(0x7f);
      return true;
    }
    if (signature.result == CompValue::OptionString || signature.result == CompValue::OptionBool ||
        signature.result == CompValue::OptionI32 || signature.result == CompValue::OptionNbtTree ||
        signature.result == CompValue::OptionMarketplaceMetadata ||
        signature.result == CompValue::ListAttributeModifier ||
        signature.result == CompValue::ListComponentValue ||
        signature.result == CompValue::ListEnchantmentValue ||
        signature.result == CompValue::ListCustomEnchantmentValue ||
        signature.result == CompValue::ListHandle || signature.result == CompValue::ListNbtTag ||
        signature.result == CompValue::ResultString) {
      // These results are written through an out-pointer the guest supplies as
      // the last canonical parameter; there is no core result.
      out->params.push_back(0x7f);
      return true;
    }
    if (signature.result == CompValue::Uuid) {
      // A 16-byte record result is passed through an out-pointer the guest
      // supplies, so the core function has no direct result.
      out->params.push_back(0x7f);
      return true;
    }
    if (signature.result == CompValue::String || signature.result == CompValue::ListString ||
        signature.result == CompValue::RecordStrings ||
        signature.result == CompValue::BlockRecord) {
      // A lifted string, list<string>, record or block result is one i32: a
      // pointer to its return area in memory 0. The host reads the pair and
      // calls post-return.
      out->has_result = true;
      out->result = 0x7f;
      return true;
    }
    if (signature.result == CompValue::ResultString)
      return fail(error, "a result<_, string> is carried by task.return, not a synchronous lift");
    if (signature.result == CompValue::ArgumentType || signature.result == CompValue::OptionF32 ||
        signature.result == CompValue::OptionF64 || signature.result == CompValue::OptionI64 ||
        signature.result == CompValue::BoundsF32 || signature.result == CompValue::BoundsF64 ||
        signature.result == CompValue::BoundsI32 || signature.result == CompValue::BoundsI64)
      return fail(error, "an argument-type value is not hosted as a result");
    if (signature.result == CompValue::BlockPos)
      return fail(error, "a block-pos result is not hosted");
    out->has_result = true;
    switch (signature.result) {
    case CompValue::I32:
      out->result = 0x7f;
      break;
    case CompValue::I64:
      out->result = 0x7e;
      break;
    case CompValue::F32:
      out->result = 0x7d;
      break;
    case CompValue::F64:
      out->result = 0x7c;
      break;
    case CompValue::Handle:
      out->result = 0x7f;
      break;
    case CompValue::String:
    case CompValue::ListString:
    case CompValue::ListU8:
    case CompValue::ListU16:
    case CompValue::Flammable:
    case CompValue::OptionFlammable:
    case CompValue::RgbColor:
    case CompValue::OptionHandle:
    case CompValue::OptionString:
    case CompValue::OptionBool:
    case CompValue::OptionI32:
    case CompValue::OptionNbtTree:
    case CompValue::AttributeModifier:
    case CompValue::ItemAttributeModifier:
    case CompValue::NbtEntry:
    case CompValue::NbtTag:
    case CompValue::NbtTree:
    case CompValue::ListNbtTag:
    case CompValue::ListAttributeModifier:
    case CompValue::EnchantmentValue:
    case CompValue::ListEnchantmentValue:
    case CompValue::CustomEnchantmentValue:
    case CompValue::ListCustomEnchantmentValue:
    case CompValue::DataComponentValue:
    case CompValue::ListComponentValue:
    case CompValue::ListHandle:
    case CompValue::RecordStrings:
    case CompValue::BlockPos:
    case CompValue::Uuid:
    case CompValue::BlockRecord:
    case CompValue::ResultString:
    case CompValue::PermissionDefault:
    case CompValue::PermissionChild:
    case CompValue::ListPermissionChild:
    case CompValue::PermissionRecord:
    case CompValue::MarketplaceMetadata:
    case CompValue::OptionMarketplaceMetadata:
    case CompValue::OptionF32:
    case CompValue::OptionF64:
    case CompValue::OptionI64:
    case CompValue::BoundsF32:
    case CompValue::BoundsF64:
    case CompValue::BoundsI32:
    case CompValue::BoundsI64:
    case CompValue::ArgumentType:
      break;
    }
  }
  return true;
}

// The core signature of an async lift is its flattened parameters and no core
// result: the value comes back through task.return.
bool flatten_async_params(const CompSignature& signature, CoreSignature* out, std::string* error) {
  CompSignature params_only = signature;
  params_only.has_result = false;
  return flatten_signature(params_only, out, error);
}

// The async lower ABI always returns one i32 status. Parameters that would
// flatten to more than four core values move behind one in-pointer; a result
// would add an out-pointer, but the hosted async members return no value.
bool flatten_async_lower(const CompSignature& signature, CoreSignature* out, std::string* error) {
  if (signature.has_result)
    return fail(error, "an async lower with a result is not hosted");
  CoreSignature params;
  if (!flatten_signature(signature, &params, error))
    return false;
  if (params.params.size() > 4)
    out->params.push_back(0x7f);
  else
    out->params = params.params;
  out->has_result = true;
  out->result = 0x7f;
  return true;
}

bool signature_matches(const CoreSignature& left, const CoreSignature& right) {
  return left.params == right.params && left.has_result == right.has_result &&
         (!left.has_result || left.result == right.result);
}

const char* host_signature(const std::string& name) {
  for (const HostFunctionRef& entry : host_surface()) {
    if (name == entry.name)
      return entry.signature;
  }
  return nullptr;
}

std::string core_export_tail(const std::string& name) {
  const std::size_t bar = name.rfind('|');
  return bar == std::string::npos ? name : name.substr(bar + 1);
}

bool find_core_export(const CoreModuleInfo& module, std::initializer_list<const char*> names,
                      std::uint32_t* index, std::string* actual) {
  for (const char* name : names) {
    const auto found = module.functions.find(name);
    if (found != module.functions.end()) {
      *index = found->second;
      *actual = name;
      return true;
    }
    for (const auto& entry : module.functions) {
      if (core_export_tail(entry.first) == name) {
        *index = entry.second;
        *actual = entry.first;
        return true;
      }
    }
  }
  return false;
}

// A compiler-produced component's core imports are named after the interface
// and member they came from. The compiled-core linker resolves every real host
// member it can carry; an unbacked declared member is bound to a trapping host
// import, never to a no-op that would invent a result.
std::string compiled_core_host(const CoreImportInfo& import) {
  const std::string signature = core_signature_text(import.signature);
  std::string module = import.module;
  if (module.rfind("cm32p2|", 0) == 0)
    module.erase(0, 7);
  const std::size_t version = module.rfind('@');
  if (version != std::string::npos)
    module.erase(version);
  if (module == "pumpkin:plugin/uuid") {
    if (import.name == "generate" && signature == "(i32)->()")
      return "uuid_generate";
    if (import.name == "parse" && signature == "(i32,i32,i32)->()")
      return "uuid_parse";
    if (import.name == "to-string" && signature == "(i64,i64,i32)->()")
      return "uuid_to_string";
  }
  if (module == "pumpkin:plugin/i18n") {
    if (import.name == "translate" && signature == "(i32,i32,i32,i32)->()")
      return "i18n_translate";
    if (import.name == "load-translations" && signature == "(i32,i32,i32,i32,i32)->()")
      return "i18n_load_translations";
  }
  if (module == "pumpkin:plugin/item-stack") {
    if (import.name == "[constructor]item-stack" && signature == "(i32,i32,i32)->(i32)")
      return "item_stack_new";
    if (import.name == "[async method]item-stack.get-registry-key" && signature == "(i32,i32)->()")
      return "item_stack_get_registry_key";
    if (import.name == "[async method]item-stack.get-count" && signature == "(i32)->(i32)")
      return "item_stack_get_count";
    if (import.name == "[async method]item-stack.set-count" && signature == "(i32,i32)->()")
      return "item_stack_set_count";
    if (import.name == "[async method]item-stack.get-max-count" && signature == "(i32)->(i32)")
      return "item_stack_get_max_count";
    if (import.name == "[async method]item-stack.clone" && signature == "(i32)->(i32)")
      return "item_stack_clone";
    if (import.name == "[async method]item-stack.with-count" && signature == "(i32,i32)->(i32)")
      return "item_stack_with_count";
    if (import.name == "[async method]item-stack.is-similar" && signature == "(i32,i32)->(i32)")
      return "item_stack_is_similar";
    if (import.name == "[async method]item-stack.get-damage" && signature == "(i32)->(i32)")
      return "item_stack_get_damage";
    if (import.name == "[async method]item-stack.set-damage" && signature == "(i32,i32)->()")
      return "item_stack_set_damage";
    if (import.name == "[async method]item-stack.get-max-damage" && signature == "(i32)->(i32)")
      return "item_stack_get_max_damage";
    if (import.name == "[async method]item-stack.is-damageable" && signature == "(i32)->(i32)")
      return "item_stack_is_damageable";
    if (import.name == "[async method]item-stack.is-unbreakable" && signature == "(i32)->(i32)")
      return "item_stack_is_unbreakable";
    if (import.name == "[async method]item-stack.set-unbreakable" && signature == "(i32,i32)->()")
      return "item_stack_set_unbreakable";
    if (import.name == "[async method]item-stack.get-repair-cost" && signature == "(i32)->(i32)")
      return "item_stack_get_repair_cost";
    if (import.name == "[async method]item-stack.set-repair-cost" && signature == "(i32,i32)->()")
      return "item_stack_set_repair_cost";
    if (import.name == "[async method]item-stack.get-item-model" && signature == "(i32,i32)->()")
      return "item_stack_get_item_model";
    if (import.name == "[async method]item-stack.set-item-model" &&
        signature == "(i32,i32,i32,i32)->()")
      return "item_stack_set_item_model";
    if (import.name == "[async method]item-stack.get-enchantment-glint-override" &&
        signature == "(i32,i32)->()")
      return "item_stack_get_enchantment_glint_override";
    if (import.name == "[async method]item-stack.set-enchantment-glint-override" &&
        signature == "(i32,i32,i32)->()")
      return "item_stack_set_enchantment_glint_override";
    if (import.name == "[async method]item-stack.get-attribute-modifiers" &&
        signature == "(i32,i32)->()")
      return "item_stack_get_attribute_modifiers";
    if (import.name == "[async method]item-stack.add-attribute-modifier" &&
        signature == "(i32,i32,i32,i32,f64,i32,i32)->()")
      return "item_stack_add_attribute_modifier";
    if (import.name == "[async method]item-stack.remove-attribute-modifiers" &&
        signature == "(i32,i32)->()")
      return "item_stack_remove_attribute_modifiers";
    if (import.name == "[async method]item-stack.clear-attribute-modifiers" &&
        signature == "(i32)->()")
      return "item_stack_clear_attribute_modifiers";
    if (import.name == "[async method]item-stack.set-custom-data" &&
        signature == "(i32,i32,i32,i32,i32,i32,i32,i32)->()")
      return "item_stack_set_custom_data";
    if (import.name == "[async method]item-stack.get-custom-data" &&
        signature == "(i32,i32,i32,i32,i32,i32)->()")
      return "item_stack_get_custom_data";
    if (import.name == "[async method]item-stack.remove-custom-data" &&
        signature == "(i32,i32,i32,i32,i32)->()")
      return "item_stack_remove_custom_data";
    if (import.name == "[async method]item-stack.has-custom-data" &&
        signature == "(i32,i32,i32,i32,i32)->(i32)")
      return "item_stack_has_custom_data";
    if (import.name == "[async method]item-stack.get-components" && signature == "(i32,i32)->()")
      return "item_stack_get_components";
    if (import.name == "[async method]item-stack.set-component" &&
        signature == "(i32,i32,i32,i32)->()")
      return "item_stack_set_component";
    if (import.name == "[async method]item-stack.remove-component" && signature == "(i32,i32)->()")
      return "item_stack_remove_component";
    if (import.name == "[async method]item-stack.get-enchantments" && signature == "(i32,i32)->()")
      return "item_stack_get_enchantments";
    if (import.name == "[async method]item-stack.add-enchantment" &&
        signature == "(i32,i32,i32)->()")
      return "item_stack_add_enchantment";
    if (import.name == "[async method]item-stack.remove-enchantment" &&
        signature == "(i32,i32)->()")
      return "item_stack_remove_enchantment";
    if (import.name == "[async method]item-stack.get-custom-enchantments" &&
        signature == "(i32,i32)->()")
      return "item_stack_get_custom_enchantments";
    if (import.name == "[async method]item-stack.add-custom-enchantment" &&
        signature == "(i32,i32,i32,i32)->()")
      return "item_stack_add_custom_enchantment";
    if (import.name == "[async method]item-stack.remove-custom-enchantment" &&
        signature == "(i32,i32,i32)->()")
      return "item_stack_remove_custom_enchantment";
    if (import.name == "[async method]item-stack.get-custom-enchantment-level" &&
        signature == "(i32,i32,i32,i32)->()")
      return "item_stack_get_custom_enchantment_level";
    if (import.name == "[async method]item-stack.has-custom-enchantment" &&
        signature == "(i32,i32,i32)->(i32)")
      return "item_stack_has_custom_enchantment";
    if (import.name == "[async method]item-stack.get-lore" && signature == "(i32,i32)->()")
      return "item_stack_get_lore";
    if (import.name == "[async method]item-stack.set-lore" && signature == "(i32,i32,i32)->()")
      return "item_stack_set_lore";
    if (import.name == "[async method]item-stack.add-lore" && signature == "(i32,i32)->()")
      return "item_stack_add_lore";
    if (import.name == "[async method]item-stack.get-custom-name" && signature == "(i32,i32)->()")
      return "item_stack_get_custom_name";
    if (import.name == "[async method]item-stack.set-custom-name" &&
        signature == "(i32,i32,i32)->()")
      return "item_stack_set_custom_name";
    if (import.name == "[resource-drop]item-stack" && signature == "(i32)->()")
      return "resource_drop_item";
  }
  if (module == "pumpkin:plugin/logging" && import.name == "log" &&
      signature == "(i32,i32,i32)->()") {
    return "pumpkin_log";
  }
  if (module == "pumpkin:plugin/scheduler") {
    if (import.name == "schedule-delayed-task" && signature == "(i32,i64)->(i32)")
      return "scheduler_schedule_delayed";
    if (import.name == "schedule-repeating-task" && signature == "(i32,i64,i64)->(i32)")
      return "scheduler_schedule_repeating";
    if (import.name == "cancel-task" && signature == "(i32)->()")
      return "scheduler_cancel";
  }
  // Pumpkin's resource methods carry an implicit resource handle as their first
  // core parameter. The handle is opaque to this host; the bindings below drop
  // it and answer the member from live state.
  if (module == "pumpkin:plugin/context") {
    if (import.name == "[method]context.get-server" && signature == "(i32)->(i32)")
      return "pumpkin_context_get_server";
    if (import.name == "[method]context.get-data-folder" && signature == "(i32,i32)->()")
      return "pumpkin_context_get_data_folder";
    if (import.name == "[method]context.register-event" && signature == "(i32,i32,i32,i32,i32)->()")
      return "pumpkin_context_register_event";
    if (import.name == "[method]context.register-command" && signature == "(i32,i32,i32,i32)->()")
      return "pumpkin_context_register_command";
    if (import.name == "[method]context.register-permission" &&
        signature == "(i32,i32,i32,i32,i32,i32,i32,i32,i32,i32)->()")
      return "pumpkin_context_register_permission";
    if (import.name == "[method]context.get-marketplace-metadata" && signature == "(i32,i32)->()")
      return "pumpkin_context_marketplace_metadata";
    if (import.name == "[resource-drop]context" && signature == "(i32)->()")
      return "resource_drop_context";
  }
  if (module == "pumpkin:plugin/command") {
    if (import.name == "[constructor]command" && signature == "(i32,i32,i32,i32)->(i32)")
      return "command_new";
    if (import.name == "[method]command.then" && signature == "(i32,i32)->()")
      return "command_then";
    if (import.name == "[method]command.execute-with-handler-id" && signature == "(i32,i32)->()")
      return "command_execute";
    if (import.name == "[static]command-node.literal" && signature == "(i32,i32)->(i32)")
      return "command_node_literal";
    if (import.name == "[static]command-node.argument" &&
        signature == "(i32,i32,i32,i32,i64,i32,i64)->(i32)")
      return "command_node_argument";
    if (import.name == "[method]command-node.then" && signature == "(i32,i32)->()")
      return "command_node_then";
    if (import.name == "[method]command-node.execute-with-handler-id" &&
        signature == "(i32,i32)->()")
      return "command_node_execute";
    if (import.name == "[method]command-node.suggest-with-handler-id" &&
        signature == "(i32,i32)->()")
      return "command_node_suggest";
    if (import.name == "[method]command-node.require-with-handler-id" &&
        signature == "(i32,i32)->()")
      return "command_node_require";
    if (import.name == "[resource-drop]command" && signature == "(i32)->()")
      return "resource_drop_command";
    if (import.name == "[resource-drop]command-node" && signature == "(i32)->()")
      return "resource_drop_command_node";
  }
  if (module == "pumpkin:plugin/server" && import.name == "[resource-drop]server" &&
      signature == "(i32)->()") {
    return "resource_drop_server";
  }
  if (module == "pumpkin:plugin/world" && import.name == "[resource-drop]world" &&
      signature == "(i32)->()") {
    return "resource_drop_world";
  }
  if (module == "pumpkin:plugin/server") {
    if (import.name == "[method]server.get-motd" && signature == "(i32,i32)->()")
      return "server_motd";
    if (import.name == "[method]server.get-player-count" && signature == "(i32)->(i32)")
      return "player_count";
    if (import.name == "[method]server.get-all-players" && signature == "(i32,i32)->()")
      return "server_all_players";
    if (import.name == "[method]server.get-world-by-name" && signature == "(i32,i32,i32,i32)->()")
      return "pumpkin_server_world_by_name";
    if (import.name == "[method]server.get-player-by-name" && signature == "(i32,i32,i32,i32)->()")
      return "pumpkin_server_player_by_name";
    if (import.name == "[method]server.get-player-count-in-world" &&
        signature == "(i32,i32)->(i32)")
      return "server_player_count_in_world";
    if (import.name == "[method]server.get-players-in-world" && signature == "(i32,i32,i32)->()")
      return "server_players_in_world";
    if (import.name == "[method]server.has-world" && signature == "(i32,i32,i32)->(i32)")
      return "pumpkin_server_has_world";
    if (import.name == "[method]server.get-max-players" && signature == "(i32)->(i32)")
      return "server_max_players";
    if (import.name == "[method]server.get-view-distance" && signature == "(i32)->(i32)")
      return "server_view_distance";
    if (import.name == "[method]server.get-simulation-distance" && signature == "(i32)->(i32)")
      return "server_simulation_distance";
    if (import.name == "[method]server.get-default-gamemode" && signature == "(i32)->(i32)")
      return "server_default_gamemode";
    if (import.name == "[method]server.get-difficulty" && signature == "(i32)->(i32)")
      return "server_difficulty";
    if (import.name == "[method]server.is-online-mode" && signature == "(i32)->(i32)")
      return "server_online_mode";
    if (import.name == "[method]server.has-whitelist" && signature == "(i32)->(i32)")
      return "server_whitelist_enabled";
    if ((import.name == "[method]server.broadcast" ||
         import.name == "[async method]server.broadcast") &&
        signature == "(i32,i32,i32)->()")
      return "pumpkin_server_broadcast";
  }
  if (module == "pumpkin:plugin/player") {
    if (import.name == "[method]player.get-id" && signature == "(i32,i32)->()")
      return "player_get_id";
    if (import.name == "[method]player.get-name" && signature == "(i32,i32)->()")
      return "player_get_name";
    if (import.name == "[method]player.get-position" && signature == "(i32,i32)->()")
      return "player_get_position";
    if (import.name == "[method]player.get-yaw" && signature == "(i32)->(f32)")
      return "player_get_yaw";
    if (import.name == "[method]player.get-pitch" && signature == "(i32)->(f32)")
      return "player_get_pitch";
    if (import.name == "[method]player.get-gamemode" && signature == "(i32)->(i32)")
      return "player_get_gamemode";
    if (import.name == "[method]player.get-health" && signature == "(i32)->(f32)")
      return "player_get_health";
    if (import.name == "[method]player.get-max-health" && signature == "(i32)->(f32)")
      return "player_get_max_health";
    if (import.name == "[method]player.get-food-level" && signature == "(i32)->(i32)")
      return "player_get_food_level";
    if (import.name == "[method]player.get-saturation" && signature == "(i32)->(f32)")
      return "player_get_saturation";
    if (import.name == "[method]player.get-exhaustion" && signature == "(i32)->(f32)")
      return "player_get_exhaustion";
    if (import.name == "[method]player.get-absorption" && signature == "(i32)->(f32)")
      return "player_get_absorption";
    if (import.name == "[method]player.get-experience-level" && signature == "(i32)->(i32)")
      return "player_get_experience_level";
    if (import.name == "[method]player.get-experience-progress" && signature == "(i32)->(f32)")
      return "player_get_experience_progress";
    if (import.name == "[method]player.get-experience-points" && signature == "(i32)->(i32)")
      return "player_get_experience_points";
    if (import.name == "[method]player.get-camera-entity-id" && signature == "(i32)->(i32)")
      return "player_get_camera_entity_id";
    if ((import.name == "[method]player.chat" || import.name == "[async method]player.chat") &&
        signature == "(i32,i32,i32)->()")
      return "pumpkin_player_chat";
    if (import.name == "[method]player.send-system-message" && signature == "(i32,i32,i32)->()")
      return "pumpkin_player_send_system_message";
    if (import.name == "[resource-drop]player" && signature == "(i32)->()")
      return "resource_drop_player";
  }
  if (module == "pumpkin:plugin/text") {
    if (import.name == "[static]text-component.text" && signature == "(i32,i32)->(i32)")
      return "pumpkin_text_text";
    if (import.name == "[method]text-component.add-text" && signature == "(i32,i32,i32)->()")
      return "pumpkin_text_add_text";
    if (import.name == "[method]text-component.add-child" && signature == "(i32,i32)->()")
      return "pumpkin_text_add_child";
    if (import.name == "[method]text-component.color-named" && signature == "(i32,i32)->()")
      return "pumpkin_text_color_named";
    if (import.name == "[method]text-component.color-rgb" && signature == "(i32,i32,i32,i32)->()")
      return "pumpkin_text_color_rgb";
    if ((import.name == "[method]text-component.bold" ||
         import.name == "[method]text-component.italic" ||
         import.name == "[method]text-component.underlined" ||
         import.name == "[method]text-component.strikethrough" ||
         import.name == "[method]text-component.obfuscated") &&
        signature == "(i32,i32)->()") {
      if (import.name.find("bold") != std::string::npos)
        return "pumpkin_text_bold";
      if (import.name.find("italic") != std::string::npos)
        return "pumpkin_text_italic";
      if (import.name.find("underlined") != std::string::npos)
        return "pumpkin_text_underlined";
      if (import.name.find("strikethrough") != std::string::npos)
        return "pumpkin_text_strikethrough";
      return "pumpkin_text_obfuscated";
    }
    if ((import.name == "[method]text-component.insertion" ||
         import.name == "[method]text-component.font") &&
        signature == "(i32,i32,i32)->()") {
      return import.name.find("font") != std::string::npos ? "pumpkin_text_font"
                                                           : "pumpkin_text_insertion";
    }
    if ((import.name == "[method]text-component.click-open-url" ||
         import.name == "[method]text-component.click-open-file" ||
         import.name == "[method]text-component.click-run-command" ||
         import.name == "[method]text-component.click-suggest-command" ||
         import.name == "[method]text-component.click-copy-to-clipboard") &&
        signature == "(i32,i32,i32)->()") {
      if (import.name.find("open-url") != std::string::npos)
        return "pumpkin_text_click_open_url";
      if (import.name.find("open-file") != std::string::npos)
        return "pumpkin_text_click_open_file";
      if (import.name.find("run-command") != std::string::npos)
        return "pumpkin_text_click_run_command";
      if (import.name.find("suggest-command") != std::string::npos)
        return "pumpkin_text_click_suggest_command";
      return "pumpkin_text_click_copy_to_clipboard";
    }
    if (import.name == "[method]text-component.click-change-page" && signature == "(i32,i32)->()")
      return "pumpkin_text_click_change_page";
    if (import.name == "[method]text-component.hover-show-text" && signature == "(i32,i32)->()")
      return "pumpkin_text_hover_show_text";
    if (import.name == "[resource-drop]text-component" && signature == "(i32)->()")
      return "resource_drop_text";
  }
  if (module == "pumpkin:plugin/world") {
    if (import.name == "[method]world.get-time-of-day" && signature == "(i32)->(i64)")
      return "world_time";
    if (import.name == "[method]world.is-raining" && signature == "(i32)->(i32)")
      return "world_is_raining";
    if (import.name == "[method]world.is-thundering" && signature == "(i32)->(i32)")
      return "world_is_thundering";
    if (import.name == "[method]world.get-block-state-id" &&
        signature == "(i32,i32,i32,i32)->(i32)")
      return "pumpkin_world_get_block";
    if (import.name == "[method]world.get-block-id" && signature == "(i32,i32,i32,i32)->(i32)")
      return "world_get_block_id";
    if (import.name == "[method]world.set-time-of-day" && signature == "(i32,i64)->()")
      return "world_set_time";
    if ((import.name == "[method]world.set-block" ||
         import.name == "[async method]world.set-block") &&
        signature == "(i32)->()")
      return "pumpkin_world_set_block_spill";
  }
  if (import.module == kHostNamespace) {
    const char* host = host_signature(import.name);
    if (host != nullptr && signature == host)
      return import.name;
  }
  std::string name = import.name;
  for (char& character : name)
    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  if (name.find("task.return") != std::string::npos ||
      name.find("task-return") != std::string::npos ||
      name.find("task_return") != std::string::npos) {
    if (signature == "(i32,i32,i32)->()")
      return "task_return";
    if (signature == "()->()")
      return "task_return_void";
  }
  if ((name.find("resource.drop") != std::string::npos ||
       name.find("resource-drop") != std::string::npos ||
       name.find("resource_drop") != std::string::npos) &&
      signature == "(i32)->()") {
    return "resource_drop";
  }
  return "component_stub";
}

bool build_compiled_core_calls(const CoreModuleInfo& module, std::vector<ExportCall>* calls,
                               std::string* error) {
  std::uint32_t load_index = 0;
  std::string load_name;
  if (!find_core_export(module, {"on-load", "on_load"}, &load_index, &load_name))
    return false;
  CoreSignature load_signature;
  if (!module.signature_of(load_index, &load_signature))
    return fail(error, "the lifecycle export on-load has no declared signature");

  bool has_task_return = false;
  for (const CoreImportInfo& import : module.imports) {
    if (compiled_core_host(import) == "task_return") {
      has_task_return = true;
      break;
    }
  }

  ExportCall load;
  load.name = "on-load";
  load.function_index = load_index;
  const bool takes_resource = load_signature.params.size() == 1 &&
                              load_signature.params[0] == 0x7f && !load_signature.has_result;
  const bool async = takes_resource && has_task_return;
  const bool sync_handle = takes_resource && !has_task_return;
  const bool callback = load_signature.params.size() == 1 && load_signature.params[0] == 0x7f &&
                        load_signature.has_result && load_signature.result == 0x7f;
  if (async) {
    load.async = true;
    load.takes_handle = true;
  } else if (sync_handle) {
    load.takes_handle = true;
    load.no_result = true;
  } else if (callback) {
    load.takes_handle = true;
    load.ignore_core_result = true;
  } else if (!load_signature.params.empty() || load_signature.has_result) {
    return fail(error, "the compiled-core lifecycle on-load is declared " +
                           core_signature_text(load_signature) +
                           "; only ()->() and (i32)->() are hosted");
  } else {
    load.no_result = true;
  }

  std::uint32_t init_index = 0;
  std::string init_name;
  const bool has_init =
      find_core_export(module, {"init-plugin", "init_plugin"}, &init_index, &init_name);
  std::uint32_t unload_index = 0;
  std::string unload_name;
  const bool has_unload =
      find_core_export(module, {"on-unload", "on_unload"}, &unload_index, &unload_name);

  if (async) {
    if (!has_init)
      return fail(error, "the async compiled-core lifecycle does not export init-plugin");
    if (!has_unload)
      return fail(error, "the async compiled-core lifecycle does not export on-unload");
  }

  if (has_init) {
    CoreSignature init_signature;
    if (!module.signature_of(init_index, &init_signature))
      return fail(error, "the lifecycle export init-plugin has no declared signature");
    if (!init_signature.params.empty() || init_signature.has_result)
      return fail(error, "the compiled-core lifecycle init-plugin is declared " +
                             core_signature_text(init_signature) + ", not ()->()");
    ExportCall init;
    init.name = "init-plugin";
    init.function_index = init_index;
    init.no_result = true;
    calls->push_back(std::move(init));
  }
  calls->push_back(std::move(load));
  if (has_unload) {
    CoreSignature unload_signature;
    if (!module.signature_of(unload_index, &unload_signature))
      return fail(error, "the lifecycle export on-unload has no declared signature");
    ExportCall unload;
    unload.name = "on-unload";
    unload.function_index = unload_index;
    if (async) {
      if (unload_signature.params.size() != 1 || unload_signature.params[0] != 0x7f ||
          unload_signature.has_result) {
        return fail(error, "the async compiled-core lifecycle on-unload is declared " +
                               core_signature_text(unload_signature) + ", not (i32)->()");
      }
      unload.async = true;
      unload.takes_handle = true;
    } else if (sync_handle) {
      if (unload_signature.params.size() != 1 || unload_signature.params[0] != 0x7f ||
          unload_signature.has_result) {
        return fail(error, "the synchronous compiled-core lifecycle on-unload is declared " +
                               core_signature_text(unload_signature) + ", not (i32)->()");
      }
      unload.takes_handle = true;
      unload.no_result = true;
    } else if (callback) {
      if (unload_signature.params.size() != 1 || unload_signature.params[0] != 0x7f ||
          !unload_signature.has_result || unload_signature.result != 0x7f) {
        return fail(error, "the callback compiled-core lifecycle on-unload is declared " +
                               core_signature_text(unload_signature) + ", not (i32)->(i32)");
      }
      unload.takes_handle = true;
      unload.ignore_core_result = true;
    } else {
      if (!unload_signature.params.empty() || unload_signature.has_result)
        return fail(error, "the compiled-core lifecycle on-unload is declared " +
                               core_signature_text(unload_signature) + ", not ()->()");
      unload.no_result = true;
    }
    calls->push_back(std::move(unload));
  }
  return true;
}

// The compiled-core linker for a real compiler artifact. It deliberately does
// not execute the component-model graph: it finds the core module that exports
// the lifecycle, binds every core import it declares, and lets the runtime
// execute that module. Backed members reach their real plusmc:host calls;
// unbacked declarations trap by name if called. This path is selected up front
// for compiler-produced components, never as a failed-strict-link downgrade.
bool prepare_compiled_core(const ComponentScan& scan, PreparedComponent* out, std::string* error) {
  // A stock v0.2 world imports the full Pumpkin surface. A compiler-produced
  // runner also imports the WASI interfaces its standard library needs.
  // Require one of those compiled shapes, so a small hand-assembled component
  // still goes through the strict linker (and is refused there).
  if (scan.instance_defs == 0 && scan.imports < 20 && !scan.wasi)
    return false;
  if (scan.modules.empty())
    return false;

  std::string last_error;
  for (const CoreModuleCandidate& candidate : scan.modules) {
    std::vector<std::uint8_t> bytes(candidate.bytes, candidate.bytes + candidate.size);
    CoreModuleInfo module;
    std::string module_error;
    if (!read_core_module(bytes, &module, &module_error)) {
      last_error = module_error;
      continue;
    }
    std::uint32_t load_index = 0;
    std::string load_name;
    if (!find_core_export(module, {"on-load", "on_load"}, &load_index, &load_name))
      continue;

    std::vector<ExportCall> calls;
    if (!build_compiled_core_calls(module, &calls, &module_error)) {
      last_error = module_error;
      continue;
    }
    // A synchronous compiler-produced handle-event has the flat tick-start
    // shape (event-id, server handle, case, tick) -> return area. A real
    // async handle-event is wider; it is left for the next ABI slice rather
    // than refusing a plugin that would otherwise load.
    std::uint32_t event_index = 0;
    std::string event_name;
    if (find_core_export(module, {"handle-event", "handle_event"}, &event_index, &event_name)) {
      CoreSignature event_signature;
      if (!module.signature_of(event_index, &event_signature)) {
        last_error = "the lifecycle module's handle-event has no declared signature";
        continue;
      }
      if (core_signature_text(event_signature) == "(i32,i32,i32,i32)->(i32)") {
        out->has_tick_callback = true;
        out->tick_callback = event_index;
      }
    }
    // `handle-task` is the scheduler callback. The compiled sync shape takes
    // the handler id and the host-issued server handle; an async no-result
    // export has the same flat core parameters and completes through
    // task.return with no result.
    std::uint32_t task_index = 0;
    std::string task_name;
    if (find_core_export(module, {"handle-task", "handle_task"}, &task_index, &task_name)) {
      CoreSignature task_signature;
      if (!module.signature_of(task_index, &task_signature)) {
        last_error = "the lifecycle module's handle-task has no declared signature";
        continue;
      }
      const std::string task_shape = core_signature_text(task_signature);
      if (task_shape != "(i32,i32)->()" && task_shape != "(i32,i32)->(i32)") {
        last_error =
            "the lifecycle module's handle-task is declared " + task_shape + ", not (i32,i32)->()";
        continue;
      }
      out->has_task_callback = true;
      out->task_callback = task_index;
      out->task_callback_status = task_shape == "(i32,i32)->(i32)";
      if (!out->task_callback_status) {
        for (const CoreImportInfo& import : module.imports) {
          if (compiled_core_host(import) == "task_return_void") {
            out->task_callback_async = true;
            break;
          }
        }
      }
    }
    const auto load_call = std::find_if(
        calls.begin(), calls.end(), [](const ExportCall& call) { return call.name == "on-load"; });
    if (load_call != calls.end() && load_call->async) {
      bool has_task_return = false;
      for (const CoreImportInfo& import : module.imports) {
        if (compiled_core_host(import) == "task_return") {
          has_task_return = true;
          break;
        }
      }
      if (!has_task_return) {
        last_error = "the async lifecycle does not import task.return";
        continue;
      }
    }

    std::uint32_t realloc = 0;
    bool has_realloc = false;
    for (const auto& entry : module.functions) {
      if (core_export_tail(entry.first) == "cabi_realloc") {
        realloc = entry.second;
        has_realloc = true;
        break;
      }
    }
    std::vector<CoreImportBinding> bindings;
    for (const CoreImportInfo& import : module.imports) {
      CoreImportBinding binding;
      binding.module = import.module;
      binding.name = import.name;
      binding.host_function = compiled_core_host(import);
      binding.signature = core_signature_text(import.signature);
      if ((binding.host_function == "i18n_translate" || binding.host_function == "uuid_to_string" ||
           binding.host_function == "item_stack_get_registry_key" ||
           binding.host_function == "player_get_name" ||
           binding.host_function == "server_all_players" ||
           binding.host_function == "server_players_in_world" ||
           binding.host_function == "item_stack_get_enchantments" ||
           binding.host_function == "item_stack_get_custom_enchantments" ||
           binding.host_function == "item_stack_get_lore" ||
           binding.host_function == "item_stack_get_item_model" ||
           binding.host_function == "item_stack_get_attribute_modifiers" ||
           binding.host_function == "item_stack_get_custom_data" ||
           binding.host_function == "item_stack_get_components" ||
           binding.host_function == "pumpkin_context_get_data_folder" ||
           binding.host_function == "pumpkin_context_marketplace_metadata" ||
           binding.host_function == "server_motd" ||
           binding.host_function == "pumpkin_context_register_permission") &&
          has_realloc) {
        binding.string_result = binding.host_function == "i18n_translate" ||
                                binding.host_function == "uuid_to_string" ||
                                binding.host_function == "item_stack_get_registry_key" ||
                                binding.host_function == "player_get_name" ||
                                binding.host_function == "server_motd" ||
                                binding.host_function == "pumpkin_context_get_data_folder";
        binding.has_realloc = true;
        binding.realloc = realloc;
      }
      bindings.push_back(std::move(binding));
    }
    out->bytes = std::move(bytes);
    out->calls = std::move(calls);
    out->bindings = std::move(bindings);
    return true;
  }
  if (!last_error.empty())
    *error = "the compiled component's lifecycle module is not hostable: " + last_error;
  return false;
}

const PinnedMemberRef* pinned_member(const PinnedInterfaceRef& interface_ref,
                                     const std::string& member_name) {
  // The pinned table names WIT members, while wit-component prefixes async
  // exports with `[async method]`. They are the same member; the async lower
  // ABI is handled separately.
  std::string normalized = member_name;
  static constexpr std::string_view async_prefix = "[async method]";
  static constexpr std::string_view method_prefix = "[method]";
  if (normalized.rfind(async_prefix, 0) == 0)
    normalized = std::string(method_prefix) + normalized.substr(async_prefix.size());
  for (std::size_t i = 0; i < interface_ref.member_count; ++i) {
    if (normalized == interface_ref.members[i].member)
      return &interface_ref.members[i];
  }
  return nullptr;
}

// Resolves a lowered member of a tracked Pumpkin interface to the plusmc:host
// call that carries it. The backed members cover zero-argument, enum/string,
// byte-list, block-record, typed-handle and plain-text calls;
// `server.get-player-by-name` writes a typed player through the guest's option
// area, and the async `player.chat`, `world.set-block` and `server.broadcast`
// members can be lowered either synchronously or through the async ABI. The real WIT still
// has many more members and interfaces; members outside this tracked table are
// refused or stubbed, not approximated.
bool pumpkin_binding(const std::string& interface_name, const std::string& member_name,
                     const CoreSignature& flattened, std::string* host, std::string* signature,
                     std::string* error) {
  // The command resource drop is a canonical built-in, not a tracked WIT
  // member; the adapter's table does not list it.
  if (member_name == "[resource-drop]command") {
    if (core_signature_text(flattened) != "(i32)->()")
      return fail(error, "[resource-drop]command flattens to " + core_signature_text(flattened));
    *host = "resource_drop_command";
    *signature = "(i32)->()";
    return true;
  }
  if (member_name == "[resource-drop]command-node") {
    if (core_signature_text(flattened) != "(i32)->()")
      return fail(error,
                  "[resource-drop]command-node flattens to " + core_signature_text(flattened));
    *host = "resource_drop_command_node";
    *signature = "(i32)->()";
    return true;
  }
  const PinnedInterfaceRef* interface_ref = find_pinned_interface(interface_name);
  if (interface_ref == nullptr) {
    return fail(error, "the component imports " + interface_name +
                           ", which is not in the tracked pumpkin:plugin@0.2.0 interface table");
  }
  const PinnedMemberRef* member = pinned_member(*interface_ref, member_name);
  if (member == nullptr) {
    return fail(error, "the component aliases " + member_name + " from " + interface_name +
                           ", which the tracked table does not list");
  }
  if (member->host == nullptr || member->host[0] == '\0') {
    const std::string_view declared = member->signature;
    std::string why = "no plusmc:host call has a compatible shape";
    if (declared.rfind("async ", 0) == 0)
      why = "the async ABI is not hosted";
    else if (declared.find("text-component") != std::string_view::npos)
      why = "text-component arguments are not hosted";
    else if (declared.find("block-pos") != std::string_view::npos)
      why = "the block-pos member shape is not hosted";
    else if (declared.find("string") != std::string_view::npos)
      why = "string arguments need the canonical-ABI string path";
    return fail(error, "the host cannot back " + interface_name + " member " + member_name + " (" +
                           member->signature + "): " + why);
  }
  std::string adapter = member->host;
  if (adapter == "world_weather") {
    if (member_name.find("is-raining") != std::string::npos)
      adapter = "world_is_raining";
    else if (member_name.find("is-thundering") != std::string::npos)
      adapter = "world_is_thundering";
  }
  std::string expected;
  if (adapter == "log_level") {
    // level: level flattens to i32; message: string to (ptr, len).
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "log_tracing") {
    // event: list<u8> flattens to (ptr, len) into memory 0.
    expected = "(i32,i32)->()";
  } else if (adapter == "world_get_block") {
    // pos: block-pos is three s32s; the host returns a u16 block state ID.
    expected = "(i32,i32,i32)->(i32)";
  } else if (adapter == "server_has_world") {
    // server handle, then name as (ptr, len); answers bool.
    expected = "(i32,i32,i32)->(i32)";
  } else if (adapter == "server_player_count_in_world") {
    // server handle plus the world handle; answers the world's player count.
    expected = "(i32,i32)->(i32)";
  } else if (adapter == "server_all_players") {
    // server handle plus the guest's list return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "server_players_in_world") {
    // server handle, world handle and the guest's list return area.
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "server_max_players" || adapter == "server_view_distance" ||
             adapter == "server_simulation_distance" || adapter == "server_default_gamemode" ||
             adapter == "server_difficulty" || adapter == "server_online_mode" ||
             adapter == "server_whitelist_enabled") {
    // Every scalar server setting is a method: the server handle in, one i32 out.
    expected = "(i32)->(i32)";
  } else if (adapter == "world_get_block_info") {
    // pos: block-pos is three s32s; the host returns a pointer to the 56-byte
    // canonical `block` return area.
    expected = "(i32,i32,i32)->(i32)";
  } else if (adapter == "world_get_block_id") {
    // world handle, then pos: block-pos's three s32s; answers the u16 type id.
    expected = "(i32,i32,i32,i32)->(i32)";
  } else if (adapter == "world_set_time") {
    // world handle plus the u64 tick count.
    expected = "(i32,i64)->()";
  } else if (adapter == "scheduler_schedule_delayed") {
    // handler id plus the u64 delay; answers the task id.
    expected = "(i32,i64)->(i32)";
  } else if (adapter == "scheduler_schedule_repeating") {
    // handler id, then the u64 delay and period; answers the task id.
    expected = "(i32,i64,i64)->(i32)";
  } else if (adapter == "scheduler_cancel") {
    expected = "(i32)->()";
  } else if (adapter == "item_stack_new") {
    // The constructor returns a resource handle; the string is (ptr, len) and
    // the count is one widened byte.
    expected = "(i32,i32,i32)->(i32)";
  } else if (adapter == "item_stack_get_registry_key") {
    // Self handle and the guest's two-word string return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_get_count" || adapter == "item_stack_get_max_count" ||
             adapter == "item_stack_clone") {
    expected = "(i32)->(i32)";
  } else if (adapter == "item_stack_set_count") {
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_with_count") {
    expected = "(i32,i32)->(i32)";
  } else if (adapter == "item_stack_is_similar") {
    expected = "(i32,i32)->(i32)";
  } else if (adapter == "item_stack_get_damage" || adapter == "item_stack_get_max_damage" ||
             adapter == "item_stack_is_damageable" || adapter == "item_stack_is_unbreakable" ||
             adapter == "item_stack_get_repair_cost") {
    expected = "(i32)->(i32)";
  } else if (adapter == "item_stack_set_damage" || adapter == "item_stack_set_unbreakable" ||
             adapter == "item_stack_set_repair_cost" ||
             adapter == "item_stack_remove_enchantment" || adapter == "item_stack_add_lore") {
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_add_enchantment") {
    // Self handle, enchantment enum and level.
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "item_stack_get_item_model" ||
             adapter == "item_stack_get_enchantment_glint_override" ||
             adapter == "item_stack_get_attribute_modifiers" ||
             adapter == "item_stack_get_components" || adapter == "item_stack_get_enchantments" ||
             adapter == "item_stack_get_custom_enchantments" || adapter == "item_stack_get_lore" ||
             adapter == "item_stack_get_custom_name") {
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_set_item_model") {
    // Self handle, then option<string>: discriminant, pointer and length.
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_set_enchantment_glint_override") {
    // Self handle, then option<bool>: discriminant and flag.
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "item_stack_add_attribute_modifier") {
    // Self handle, attribute enum, modifier id (ptr, len), f64 amount,
    // modifier operation and slot enum.
    expected = "(i32,i32,i32,i32,f64,i32,i32)->()";
  } else if (adapter == "item_stack_remove_attribute_modifiers") {
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_clear_attribute_modifiers") {
    expected = "(i32)->()";
  } else if (adapter == "item_stack_set_custom_data") {
    // Self, namespace and key as (ptr, len), then nbt-tree's root and
    // (tags pointer, tags length).
    expected = "(i32,i32,i32,i32,i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_get_custom_data") {
    // Self, namespace (ptr, len), key (ptr, len), option<nbt-tree> area.
    expected = "(i32,i32,i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_remove_custom_data") {
    expected = "(i32,i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_has_custom_data") {
    expected = "(i32,i32,i32,i32,i32)->(i32)";
  } else if (adapter == "item_stack_set_component") {
    // Self, component enum, value (ptr, len).
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_remove_component") {
    expected = "(i32,i32)->()";
  } else if (adapter == "item_stack_add_custom_enchantment") {
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_remove_custom_enchantment" ||
             adapter == "item_stack_set_lore" || adapter == "item_stack_set_custom_name") {
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "item_stack_get_custom_enchantment_level") {
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "item_stack_has_custom_enchantment") {
    expected = "(i32,i32,i32)->(i32)";
  } else if (adapter == "resource_drop_item") {
    expected = "(i32)->()";
  } else if (adapter == "server_motd") {
    // Server handle and the guest's two-word string return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "player_get_id") {
    // Self handle and the guest's 16-byte uuid return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "player_get_name") {
    // Self handle and the guest's two-word string return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "player_get_position") {
    // Self handle and the 24-byte position return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "player_get_yaw" || adapter == "player_get_pitch") {
    // Self handle in, f32 orientation out.
    expected = "(i32)->(f32)";
  } else if (adapter == "player_get_gamemode" || adapter == "player_get_experience_level" ||
             adapter == "player_get_experience_points") {
    // Self handle in, enum or experience value out.
    expected = "(i32)->(i32)";
  } else if (adapter == "player_get_health" || adapter == "player_get_max_health" ||
             adapter == "player_get_saturation" || adapter == "player_get_exhaustion" ||
             adapter == "player_get_absorption" || adapter == "player_get_experience_progress") {
    // Self handle in, f32 health, vitals or experience progress out.
    expected = "(i32)->(f32)";
  } else if (adapter == "player_get_food_level") {
    // Self handle in, food level (u8 flattened to i32) out.
    expected = "(i32)->(i32)";
  } else if (adapter == "player_get_camera_entity_id") {
    // Self handle in, u32 entity id out.
    expected = "(i32)->(i32)";
  } else if (adapter == "i18n_translate") {
    // key (ptr, len), locale enum, and the guest's string return area.
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "i18n_load_translations") {
    // namespace (ptr, len), json (ptr, len), locale enum.
    expected = "(i32,i32,i32,i32,i32)->()";
  } else if (adapter == "uuid_to_string") {
    // The uuid record's two u64 halves and the string return area.
    expected = "(i64,i64,i32)->()";
  } else if (adapter == "uuid_generate") {
    // The guest supplies the 16-byte uuid return area.
    expected = "(i32)->()";
  } else if (adapter == "player_chat") {
    // message: string flattens to (ptr, len).
    expected = "(i32,i32)->()";
  } else if (adapter == "server_broadcast") {
    // server handle, then message: string as (ptr, len).
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "server_player_by_name") {
    // server handle, name as (ptr, len), and the guest's option<player> area.
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "player_send_system_message" || adapter == "text_add_text") {
    // The player handle, text handle and overlay bool; add-text takes the text
    // handle followed by the string's (ptr, len).
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "text_text") {
    // plain: string -> a typed text-component handle.
    expected = "(i32,i32)->(i32)";
  } else if (adapter == "text_add_child") {
    // parent text handle plus the child text handle.
    expected = "(i32,i32)->()";
  } else if (adapter == "text_color_named" || adapter == "text_bold" || adapter == "text_italic" ||
             adapter == "text_underlined" || adapter == "text_strikethrough" ||
             adapter == "text_obfuscated" || adapter == "text_click_change_page" ||
             adapter == "text_hover_show_text") {
    // Self handle plus an enum discriminant, bool, page number or child text
    // handle.
    expected = "(i32,i32)->()";
  } else if (adapter == "text_color_rgb") {
    // Self handle plus the three widened rgb-color fields.
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "text_insertion" || adapter == "text_font" ||
             adapter == "text_click_open_url" || adapter == "text_click_open_file" ||
             adapter == "text_click_run_command" || adapter == "text_click_suggest_command" ||
             adapter == "text_click_copy_to_clipboard") {
    // Self handle plus a string as (ptr, len).
    expected = "(i32,i32,i32)->()";
  } else if (adapter == "pumpkin_context_get_server") {
    // The owned context handle, answered with a server handle.
    expected = "(i32)->(i32)";
  } else if (adapter == "pumpkin_context_register_event") {
    // context handle, handler id, event-type discriminant, priority
    // discriminant and blocking flag.
    expected = "(i32,i32,i32,i32,i32)->()";
  } else if (adapter == "pumpkin_context_get_data_folder") {
    // Context handle and the string return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "pumpkin_context_register_permission") {
    expected = "(i32,i32,i32,i32,i32,i32,i32,i32,i32,i32)->()";
  } else if (adapter == "pumpkin_context_register_command") {
    // context handle, the command resource handle and the permission string.
    expected = "(i32,i32,i32,i32)->()";
  } else if (adapter == "pumpkin_context_marketplace_metadata") {
    // context handle and the option<marketplace-metadata> return area.
    expected = "(i32,i32)->()";
  } else if (adapter == "command_new") {
    // names (pointer, length), description (pointer, length), handle result.
    expected = "(i32,i32,i32,i32)->(i32)";
  } else if (adapter == "command_execute") {
    // command handle and handler id.
    expected = "(i32,i32)->()";
  } else if (adapter == "command_then") {
    // command handle and command-node handle.
    expected = "(i32,i32)->()";
  } else if (adapter == "command_node_literal") {
    // name (pointer, length), node handle result.
    expected = "(i32,i32)->(i32)";
  } else if (adapter == "command_node_argument") {
    // name (pointer, length), argument-type's discriminant and four joined
    // payload slots (i32, i64, i32, i64), node handle result.
    expected = "(i32,i32,i32,i32,i64,i32,i64)->(i32)";
  } else if (adapter == "command_node_then" || adapter == "command_node_execute" ||
             adapter == "command_node_suggest" || adapter == "command_node_require") {
    // node handle and either the child handle or a handler id.
    expected = "(i32,i32)->()";
  } else if (adapter == "pumpkin_world_set_block") {
    // pos (x,y,z), block record pointer, update-flags.
    expected = "(i32,i32,i32,i32,i32)->()";
  } else if (member->signature == std::string("func() -> u32") ||
             member->signature == std::string("func() -> bool")) {
    expected = "()->(i32)";
  } else if (member->signature == std::string("func() -> u64")) {
    expected = "()->(i64)";
  } else {
    return fail(error, "the tracked member " + interface_name + " " + member_name +
                           " has a shape the linker does not lower");
  }
  if (core_signature_text(flattened) != expected) {
    return fail(error, member_name + " flattens to " + core_signature_text(flattened) +
                           " but its tracked signature is " + expected);
  }
  *host = adapter;
  *signature = expected;
  return true;
}

// Resolves a lift's core function index to a function the module defines. A
// lift names a core alias of the instantiated module; in the minimal shape it
// names the module's function directly.
bool resolve_lift(const Shape& shape, const CoreModuleInfo& module, std::uint32_t module_instance,
                  std::uint32_t core_index, std::uint32_t* function, std::string* error) {
  if (shape.minimal()) {
    *function = core_index;
    return true;
  }
  if (core_index >= shape.core_funcs.size())
    return fail(error, "a canon lift names core function " + std::to_string(core_index) +
                           ", which is not a core alias of the module");
  const CoreFuncEntry& entry = shape.core_funcs[core_index];
  if (!entry.alias)
    return fail(error, "a canon lift names a canon lower, not a core alias of the module");
  if (entry.instance != module_instance)
    return fail(error, "a canon lift aliases core instance " + std::to_string(entry.instance) +
                           ", not the instance that holds the module");
  const auto found = module.functions.find(entry.name);
  if (found == module.functions.end())
    return fail(error, "a canon lift aliases " + entry.name + ", which the module does not export");
  *function = found->second;
  return true;
}

bool comp_value_needs_memory(CompValue value) {
  switch (value) {
  case CompValue::String:
  case CompValue::ListString:
  case CompValue::ListU8:
  case CompValue::OptionString:
  case CompValue::OptionNbtTree:
  case CompValue::AttributeModifier:
  case CompValue::ItemAttributeModifier:
  case CompValue::NbtTree:
  case CompValue::ListNbtTag:
  case CompValue::ListAttributeModifier:
  case CompValue::EnchantmentValue:
  case CompValue::ListEnchantmentValue:
  case CompValue::CustomEnchantmentValue:
  case CompValue::ListCustomEnchantmentValue:
  case CompValue::DataComponentValue:
  case CompValue::ListComponentValue:
  case CompValue::ListHandle:
  case CompValue::BlockRecord:
  case CompValue::PermissionRecord:
  case CompValue::PermissionChild:
  case CompValue::ListPermissionChild:
  case CompValue::MarketplaceMetadata:
  case CompValue::OptionMarketplaceMetadata:
  case CompValue::ResultString:
  case CompValue::ArgumentType:
    return true;
  default:
    return false;
  }
}

bool comp_signature_needs_memory(const CompSignature& signature) {
  for (const CompValue value : signature.params)
    if (comp_value_needs_memory(value))
      return true;
  if (signature.has_result &&
      (comp_value_needs_memory(signature.result) || signature.result == CompValue::OptionBool ||
       signature.result == CompValue::OptionI32))
    return true;
  return false;
}

bool check_realloc_option(const CoreFuncEntry& lowered, const CoreModuleInfo& module,
                          const std::string& member, std::string* error) {
  if (!lowered.options.has_realloc)
    return fail(error, "the component lowers " + member +
                           " with an allocating result without a realloc canonical option");
  if (lowered.options.realloc >= module.function_count())
    return fail(error, "the component lowers " + member + " with realloc function " +
                           std::to_string(lowered.options.realloc) +
                           ", which the module does not define");
  if (lowered.options.realloc < module.imports.size())
    return fail(error, "the component lowers " + member + " with realloc function " +
                           std::to_string(lowered.options.realloc) +
                           ", which is imported; cabi_realloc must be defined by the module");
  CoreSignature realloc_signature;
  if (!module.signature_of(lowered.options.realloc, &realloc_signature) ||
      realloc_signature.params != std::vector<std::uint8_t>{0x7f, 0x7f, 0x7f, 0x7f} ||
      !realloc_signature.has_result || realloc_signature.result != 0x7f) {
    return fail(error, "the component lowers " + member + " with realloc function " +
                           std::to_string(lowered.options.realloc) +
                           ", which is not declared (i32,i32,i32,i32)->(i32)");
  }
  return true;
}

bool link_shape(const Shape& shape, const CoreModuleInfo& module, std::vector<ExportCall>* calls,
                std::vector<CoreImportBinding>* bindings, std::string* error) {
  std::uint32_t module_instance = 0;
  bool has_module_instance = false;
  for (std::uint32_t i = 0; i < shape.core_instances.size(); ++i) {
    if (shape.core_instances[i].instantiate && shape.core_instances[i].module == 0) {
      module_instance = i;
      has_module_instance = true;
      break;
    }
  }

  for (const CoreImportInfo& import : module.imports) {
    std::string host_name = import.name;
    std::string signature;
    bool string_result = false;
    bool list_result = false;
    bool record_result = false;
    bool block_result = false;
    bool has_realloc = false;
    std::uint32_t realloc = 0;
    bool async_lower = false;
    // The compound-result forms are only bound through a canon lower: a direct
    // core import has no realloc option, so the runtime could not hand the
    // value back. Resolve them through the component instance below.
    if (import.module == kHostNamespace && import.name != "config_get_string" &&
        import.name != "config_keys" && import.name != "config_pair") {
      const char* direct = host_signature(import.name);
      if (direct == nullptr)
        return fail(error, "the module imports " + import.module + "/" + import.name +
                               ", which the host does not provide");
      signature = direct;
    } else if (import.module == "pumpkin:plugin/logging" && import.name == "log") {
      // Pumpkin's logging interface is the one v0.2 host import this slice
      // carries directly: `log(level, message)`. The component-level instance
      // import is not needed to instantiate a module that only calls it.
      signature = "(i32,i32,i32)->()";
      host_name = "pumpkin_log";
    } else {
      if (!has_module_instance)
        return fail(error, "the module imports " + import.module + "/" + import.name +
                               " but the component does not instantiate it");
      const CoreInstanceEntry* provider = nullptr;
      for (const auto& argument : shape.core_instances[module_instance].args) {
        if (argument.first == import.module) {
          provider = &shape.core_instances[argument.second];
          break;
        }
      }
      if (provider == nullptr)
        return fail(error, "the module imports " + import.module + "/" + import.name +
                               " but the component passes no instance for " + import.module);
      if (provider->instantiate)
        return fail(error, "the instance passed for " + import.module +
                               " is a core instantiation, not a synthetic export list");
      const CoreExportEntry* exported = nullptr;
      for (const CoreExportEntry& entry : provider->exports) {
        if (entry.name == import.name) {
          exported = &entry;
          break;
        }
      }
      if (exported == nullptr)
        return fail(error,
                    "the instance passed for " + import.module + " does not export " + import.name);
      if (exported->sort != 0x00)
        return fail(error, import.module + "/" + import.name +
                               " is bound to a core definition "
                               "that is not a function");
      if (exported->index >= shape.core_funcs.size())
        return fail(error, import.module + "/" + import.name +
                               " is bound to a core function that is not defined");
      const CoreFuncEntry& lowered = shape.core_funcs[exported->index];
      if (lowered.alias)
        return fail(error, import.module + "/" + import.name +
                               " is bound to a core alias, not a canonical definition");
      if (lowered.kind == CoreFuncEntry::Kind::ResourceDrop) {
        signature = "(i32)->()";
        host_name = "resource_drop";
      } else if (lowered.kind == CoreFuncEntry::Kind::TaskReturn) {
        if (!lowered.task_result.has_result) {
          signature = "()->()";
          host_name = "task_return_void";
        } else {
          if (lowered.task_result.result != CompValue::ResultString)
            return fail(error, "task.return is not carrying result<_, string>");
          if (!module.has_memory)
            return fail(error, "task.return carries a string result but the module has no memory");
          signature = "(i32,i32,i32)->()";
          host_name = "task_return";
        }
      } else if (lowered.kind == CoreFuncEntry::Kind::WaitableSetNew) {
        signature = "()->(i32)";
        host_name = "waitable_set_new";
      } else if (lowered.kind == CoreFuncEntry::Kind::WaitableSetJoin) {
        signature = "(i32,i32)->()";
        host_name = "waitable_set_join";
      } else if (lowered.kind == CoreFuncEntry::Kind::WaitableSetWait) {
        if (!module.has_memory || !lowered.options.has_memory || lowered.options.memory != 0) {
          return fail(error, "waitable-set.wait must name the module's memory 0");
        }
        signature = "(i32,i32)->(i32)";
        host_name = "waitable_set_wait";
      } else {
        if (lowered.component_func >= shape.funcs.size())
          return fail(error, "canon lower names an undefined component function");
        const CompFuncEntry& target = shape.funcs[lowered.component_func];
        if (target.kind != CompFuncEntry::Kind::Alias)
          return fail(error, import.module + "/" + import.name +
                                 " is bound to a canon lower of an import that is not an "
                                 "instance member alias");
        if (target.instance >= shape.instances.size())
          return fail(error, "the lowered function aliases an undefined component instance");
        const CompInstanceEntry& source = shape.instances[target.instance];
        if (target.type_index >= shape.types.size())
          return fail(error, "the lowered function has no type");
        const CompSignature& component_signature = shape.types[target.type_index].func;
        if (lowered.options.async) {
          if (component_signature.has_result)
            return fail(error, "an async lower with a result is not hosted");
          CoreSignature async_flat;
          if (!flatten_async_lower(component_signature, &async_flat, error))
            return false;
          if (!signature_matches(async_flat, import.signature)) {
            return fail(error, target.member + " lowers to " + core_signature_text(async_flat) +
                                   " async but " + import.module + "/" + import.name +
                                   " is imported as " + core_signature_text(import.signature));
          }
          if (comp_signature_needs_memory(component_signature) &&
              (!lowered.options.has_memory || lowered.options.memory != 0))
            return fail(error, "the component lowers " + target.member +
                                   " async without the module's memory 0");
          if (lowered.options.has_realloc || lowered.options.has_post_return)
            return fail(error, "an async lower cannot carry realloc or post-return");
          if (source.name != kHostNamespace) {
            if (target.member == "[method]player.chat")
              host_name = "player_chat";
            else if (target.member == "[method]world.set-block")
              host_name = "pumpkin_world_set_block_async";
            else if (target.member == "[method]server.broadcast")
              host_name = "pumpkin_server_broadcast";
            else
              return fail(error, "the async lower " + target.member + " is not hosted");
          } else if (target.member == "player_chat" || target.member == "server_broadcast") {
            host_name = target.member;
          } else {
            return fail(error, "the async lower " + target.member + " is not hosted");
          }
          signature = core_signature_text(async_flat);
          async_lower = true;
          CoreImportBinding binding;
          binding.module = import.module;
          binding.name = import.name;
          binding.host_function = host_name;
          binding.signature = signature;
          binding.async_lower = true;
          bindings->push_back(std::move(binding));
          continue;
        }
        CoreSignature flattened;
        if (!flatten_signature(component_signature, &flattened, error))
          return false;
        // A lowered string result is a return-pointer parameter, not a core
        // result. `flatten_signature` describes the lift convention (the core
        // function returns the return area's pointer); a canon lower hands the
        // guest's return area to the host as one extra i32 parameter. The
        // tracked string members (`get-registry-key`, `player.get-name`) use
        // that host signature.
        CoreSignature lowered_flat = flattened;
        if (source.name != kHostNamespace && component_signature.has_result &&
            component_signature.result == CompValue::String) {
          lowered_flat.params.push_back(0x7f);
          lowered_flat.has_result = false;
        }
        if (!signature_matches(lowered_flat, import.signature)) {
          return fail(error, target.member + " flattens to " + core_signature_text(lowered_flat) +
                                 " but " + import.module + "/" + import.name + " is imported as " +
                                 core_signature_text(import.signature));
        }
        if (comp_signature_needs_memory(component_signature) && !lowered.options.has_memory)
          return fail(error, "the component lowers " + target.member +
                                 ", which takes or returns a pointer-bearing value, without "
                                 "a memory canonical option");
        if (lowered.options.has_memory && lowered.options.memory != 0)
          return fail(error, "the component lowers " + target.member + " against core memory " +
                                 std::to_string(lowered.options.memory) +
                                 "; only the module's memory 0 is hosted");
        if (lowered.options.has_memory && !module.has_memory)
          return fail(error, "the component lowers " + target.member +
                                 " with a memory option but "
                                 "the module has no memory");
        const bool returns_string =
            component_signature.has_result && component_signature.result == CompValue::String;
        const bool returns_list =
            component_signature.has_result && component_signature.result == CompValue::ListString;
        const bool returns_record = component_signature.has_result &&
                                    component_signature.result == CompValue::RecordStrings;
        const bool returns_block =
            component_signature.has_result && component_signature.result == CompValue::BlockRecord;
        const bool returns_indirect =
            component_signature.has_result &&
            (component_signature.result == CompValue::OptionString ||
             component_signature.result == CompValue::OptionBool ||
             component_signature.result == CompValue::OptionI32 ||
             component_signature.result == CompValue::OptionNbtTree ||
             component_signature.result == CompValue::ListAttributeModifier ||
             component_signature.result == CompValue::ListComponentValue ||
             component_signature.result == CompValue::ListEnchantmentValue ||
             component_signature.result == CompValue::ListCustomEnchantmentValue ||
             component_signature.result == CompValue::ListHandle ||
             component_signature.result == CompValue::ListNbtTag ||
             component_signature.result == CompValue::ResultString ||
             component_signature.result == CompValue::OptionMarketplaceMetadata);
        const bool indirect_allocates =
            component_signature.has_result &&
            (component_signature.result == CompValue::OptionString ||
             component_signature.result == CompValue::OptionNbtTree ||
             component_signature.result == CompValue::ListAttributeModifier ||
             component_signature.result == CompValue::ListComponentValue ||
             component_signature.result == CompValue::ListEnchantmentValue ||
             component_signature.result == CompValue::ListCustomEnchantmentValue ||
             component_signature.result == CompValue::ListHandle ||
             component_signature.result == CompValue::ListNbtTag ||
             component_signature.result == CompValue::ResultString ||
             component_signature.result == CompValue::OptionMarketplaceMetadata);
        if (returns_string && source.name != kHostNamespace) {
          // A tracked Pumpkin member can return a string: the host writes the
          // canonical (pointer, length) pair into the guest's return area and
          // allocates the bytes through the module's cabi_realloc. This is the
          // path `item-stack.get-registry-key`, `i18n.translate` and
          // `player.get-name` use; it is not the direct plusmc:host
          // `config_get_string` return-pointer form below.
          std::string host;
          std::string host_signature_text;
          if (!pumpkin_binding(source.name, target.member, lowered_flat, &host,
                               &host_signature_text, error))
            return false;
          if (!module.has_memory)
            return fail(error, target.member + " returns a string but the module has no memory");
          if (!lowered.options.has_memory || lowered.options.memory != 0)
            return fail(error, "the component lowers " + target.member +
                                   " with a string result against core memory " +
                                   std::to_string(lowered.options.memory) +
                                   "; only the module's memory 0 is hosted");
          if (!check_realloc_option(lowered, module, target.member, error))
            return false;
          signature = host_signature_text;
          host_name = std::move(host);
          has_realloc = true;
          realloc = lowered.options.realloc;
        } else if (returns_string || returns_list || returns_record) {
          if (source.name != kHostNamespace)
            return fail(error, target.member +
                                   " returns a compound value; only a direct plusmc:host "
                                   "import is hosted with a string, list or record result");
          std::string expected;
          std::string shape;
          if (returns_string) {
            expected = "config_get_string";
            shape = "a string result";
          } else if (returns_list) {
            expected = "config_keys";
            shape = "a list<string> result";
          } else {
            expected = "config_pair";
            shape = "a record result";
          }
          if (target.member != expected) {
            return fail(error, "the component lowers " + target.member + " with " + shape +
                                   "; only plusmc:host/" + expected + " is hosted");
          }
          if (!module.has_memory)
            return fail(error,
                        target.member + " returns a compound value but the module has no memory");
          if (!lowered.options.has_memory || lowered.options.memory != 0)
            return fail(error, "the component lowers " + target.member +
                                   " with a compound result against core memory " +
                                   std::to_string(lowered.options.memory) +
                                   "; only the module's memory 0 is hosted");
          if (!lowered.options.has_realloc)
            return fail(error, "the component lowers " + target.member +
                                   " with a compound result without a realloc canonical "
                                   "option");
          if (lowered.options.realloc >= module.function_count())
            return fail(error, "the component lowers " + target.member + " with realloc function " +
                                   std::to_string(lowered.options.realloc) +
                                   ", which the module does not define");
          if (lowered.options.realloc < module.imports.size())
            return fail(error, "the component lowers " + target.member + " with realloc function " +
                                   std::to_string(lowered.options.realloc) +
                                   ", which is imported; cabi_realloc must be defined by the "
                                   "module");
          CoreSignature realloc_signature;
          if (!module.signature_of(lowered.options.realloc, &realloc_signature) ||
              realloc_signature.params != std::vector<std::uint8_t>{0x7f, 0x7f, 0x7f, 0x7f} ||
              !realloc_signature.has_result || realloc_signature.result != 0x7f) {
            return fail(error, "the component lowers " + target.member + " with realloc function " +
                                   std::to_string(lowered.options.realloc) +
                                   ", which is not declared (i32,i32,i32,i32)->(i32)");
          }
          if (!known_host_function(target.member) || host_signature(target.member) == nullptr)
            return fail(error, "the component lowers " + target.member +
                                   ", which the host does not provide");
          signature = host_signature(target.member);
          host_name = target.member;
          string_result = returns_string;
          list_result = returns_list;
          record_result = returns_record;
          has_realloc = true;
          realloc = lowered.options.realloc;
        } else if (source.name == kHostNamespace) {
          if (!known_host_function(target.member))
            return fail(error, "the component lowers " + target.member +
                                   ", which the host does not provide");
          const char* host = host_signature(target.member);
          if (host == nullptr)
            return fail(error, "the component lowers " + target.member +
                                   ", which the host does not provide");
          signature = host;
          host_name = target.member;
        } else {
          std::string host;
          if (!pumpkin_binding(source.name, target.member, lowered_flat, &host, &signature, error))
            return false;
          host_name = std::move(host);
          if (returns_block) {
            if (target.member != "[method]world.get-block")
              return fail(error, "the component lowers " + target.member +
                                     " with a block result; only world.get-block is hosted");
            if (!module.has_memory)
              return fail(error, target.member + " returns a block but the module has no memory");
            if (!lowered.options.has_memory || lowered.options.memory != 0)
              return fail(error, "the component lowers " + target.member +
                                     " with a block result against core memory " +
                                     std::to_string(lowered.options.memory) +
                                     "; only the module's memory 0 is hosted");
            if (!lowered.options.has_realloc)
              return fail(error, "the component lowers " + target.member +
                                     " with a block result without a realloc canonical option");
            if (lowered.options.realloc >= module.function_count())
              return fail(error, "the component lowers " + target.member +
                                     " with realloc function " +
                                     std::to_string(lowered.options.realloc) +
                                     ", which the module does not define");
            if (lowered.options.realloc < module.imports.size())
              return fail(error, "the component lowers " + target.member +
                                     " with realloc function " +
                                     std::to_string(lowered.options.realloc) +
                                     ", which is imported; cabi_realloc must be defined by the "
                                     "module");
            CoreSignature realloc_signature;
            if (!module.signature_of(lowered.options.realloc, &realloc_signature) ||
                realloc_signature.params != std::vector<std::uint8_t>{0x7f, 0x7f, 0x7f, 0x7f} ||
                !realloc_signature.has_result || realloc_signature.result != 0x7f) {
              return fail(error, "the component lowers " + target.member +
                                     " with realloc function " +
                                     std::to_string(lowered.options.realloc) +
                                     ", which is not declared (i32,i32,i32,i32)->(i32)");
            }
            block_result = true;
            has_realloc = true;
            realloc = lowered.options.realloc;
          } else if (returns_indirect) {
            if (!module.has_memory)
              return fail(error, target.member + " returns an indirect value but the module "
                                                 "has no memory");
            if (!lowered.options.has_memory || lowered.options.memory != 0)
              return fail(error, "the component lowers " + target.member +
                                     " with an indirect result against core memory " +
                                     std::to_string(lowered.options.memory) +
                                     "; only the module's memory 0 is hosted");
            if (indirect_allocates) {
              if (!check_realloc_option(lowered, module, target.member, error))
                return false;
              has_realloc = true;
              realloc = lowered.options.realloc;
            }
          }
        }
      }
    }
    CoreImportBinding binding;
    binding.module = import.module;
    binding.name = import.name;
    binding.host_function = host_name;
    binding.signature = signature;
    binding.string_result = string_result;
    binding.list_result = list_result;
    binding.record_result = record_result;
    binding.block_result = block_result;
    binding.has_realloc = has_realloc;
    binding.realloc = realloc;
    binding.async_lower = async_lower;
    bindings->push_back(binding);
  }

  // wit-component numbers a pure-lift component's functions 0, 2, 4, ...;
  // components with imported/aliased component functions use the compact
  // 0, 1, 2 numbering. Detect the former from the whole shape so every
  // lifecycle lift is folded consistently, not just the out-of-range ones.
  bool spaced_lifts =
      !shape.funcs.empty() &&
      std::all_of(shape.funcs.begin(), shape.funcs.end(), [](const CompFuncEntry& entry) {
        return entry.kind == CompFuncEntry::Kind::Lift;
      });
  bool spaced_out_of_range = false;
  if (spaced_lifts) {
    for (const CompExportEntry& entry : shape.exports) {
      if (entry.core || entry.sort != 0x01)
        continue;
      if (entry.index % 2 != 0)
        spaced_lifts = false;
      if (entry.index >= shape.funcs.size())
        spaced_out_of_range = true;
    }
    spaced_lifts = spaced_lifts && spaced_out_of_range;
  }

  std::vector<ExportCall> found;
  for (const CompExportEntry& export_entry : shape.exports) {
    const std::string& name = export_entry.name;
    if (name != "init-plugin" && name != "on-load" && name != "on-enable" && name != "on-disable" &&
        name != "on-unload") {
      continue;
    }
    if (export_entry.core || export_entry.sort != 0x01) {
      return fail(error, "the component exports " + name + " (" + sort_name(export_entry) +
                             "), not a function");
    }
    std::uint32_t function_index = export_entry.index;
    if (spaced_lifts)
      function_index /= 2;
    if (function_index >= shape.funcs.size())
      return fail(error, "the component exports " + name + " as function " +
                             std::to_string(export_entry.index) +
                             ", which does not name a lifted function: it defines " +
                             std::to_string(shape.funcs.size()) + " function(s)");
    const CompFuncEntry& function = shape.funcs[function_index];
    if (function.kind != CompFuncEntry::Kind::Lift)
      return fail(error,
                  "the component exports " + name + " as a function that is not a canon lift");
    if (function.type_index >= shape.types.size())
      return fail(error, "the component exports " + name + " with no type");
    const CompSignature& signature = shape.types[function.type_index].func;
    const CanonOptions& options = function.options;
    const bool v02_lifecycle = (name == "on-load" || name == "on-unload") &&
                               signature.params.size() == 1 &&
                               signature.params[0] == CompValue::Handle && signature.has_result &&
                               signature.result == CompValue::ResultString;
    if (!v02_lifecycle && !signature.params.empty()) {
      return fail(error, "the component exports " + name +
                             " with parameters; only Pumpkin v0.2's owned context handle is "
                             "hosted");
    }
    if (v02_lifecycle && (!signature.async || !options.async)) {
      return fail(error, "the v0.2 lifecycle export " + name +
                             " must be an async lift of an async function type");
    }
    if (!v02_lifecycle && (signature.async || options.async)) {
      return fail(error, "the async lifecycle export " + name +
                             " does not carry the v0.2 context/result<_, string> shape");
    }
    if (name == "init-plugin" && signature.has_result) {
      return fail(error, "init-plugin must not return a value");
    }
    const bool string_result =
        !v02_lifecycle && signature.has_result && signature.result == CompValue::String;
    if (string_result && !module.has_memory) {
      return fail(error, "the lifecycle lift for " + name +
                             " returns a string but the module has no memory");
    }
    if (options.has_memory && (options.memory != 0 || !module.has_memory)) {
      return fail(error, "the lifecycle lift for " + name + " names memory " +
                             std::to_string(options.memory) +
                             "; the hosted shape binds the module's memory 0");
    }
    if (options.has_realloc && options.realloc >= module.function_count()) {
      return fail(error, "the lifecycle lift for " + name + " names realloc function " +
                             std::to_string(options.realloc) +
                             ", which the module does not define");
    }
    if (options.has_post_return && options.post_return >= module.function_count()) {
      return fail(error, "the lifecycle lift for " + name + " names post-return function " +
                             std::to_string(options.post_return) +
                             ", which the module does not define");
    }
    std::uint32_t core_function = 0;
    if (!resolve_lift(shape, module, module_instance, function.core_index, &core_function, error))
      return false;
    CoreSignature actual;
    if (!module.signature_of(core_function, &actual)) {
      return fail(error, "the lifecycle lift for " + name + " names core function " +
                             std::to_string(core_function) + ", which has no declared signature");
    }
    if (!v02_lifecycle && !actual.params.empty()) {
      return fail(error, "the lifecycle entry " + name +
                             " takes parameters; this ABI calls "
                             "it with none");
    }
    CoreSignature expected;
    ExportCall call;
    call.name = name;
    call.function_index = core_function;
    if (v02_lifecycle) {
      if (!flatten_async_params(signature, &expected, error))
        return false;
      call.async = true;
      call.takes_handle = true;
    } else if (string_result) {
      // The core function returns one i32: the (pointer, length) return area.
      expected.has_result = true;
      expected.result = 0x7f;
      call.string_result = true;
      call.has_post_return = options.has_post_return;
      call.post_return = options.post_return;
    } else {
      // The original +MC lifecycle lifts do not require a core result; the
      // machine reads an empty stack as zero. Accept either that shape or an
      // explicit i32, while init-plugin is always ()->().
      expected.has_result = false;
      call.no_result = name == "init-plugin";
    }
    bool matches = signature_matches(actual, expected);
    if (!v02_lifecycle && !string_result && name != "init-plugin") {
      CoreSignature explicit_zero;
      explicit_zero.has_result = true;
      explicit_zero.result = 0x7f;
      matches = matches || signature_matches(actual, explicit_zero);
    }
    if (!matches) {
      return fail(error, "the lifecycle lift for " + name + " expects core signature " +
                             core_signature_text(expected) + " but the module exports " +
                             core_signature_text(actual));
    }
    found.push_back(std::move(call));
  }

  const auto find = [&found](const std::string& name) -> const ExportCall* {
    for (const ExportCall& call : found) {
      if (call.name == name)
        return &call;
    }
    return nullptr;
  };
  const ExportCall* load = find("on-load");
  if (load == nullptr)
    return fail(error, "the component does not export on-load");
  if (load->async) {
    const ExportCall* init = find("init-plugin");
    const ExportCall* unload = find("on-unload");
    if (init == nullptr)
      return fail(error, "the v0.2 component does not export init-plugin");
    if (unload == nullptr)
      return fail(error, "the v0.2 component does not export on-unload");
    calls->push_back(*init);
    calls->push_back(*load);
    calls->push_back(*unload);
  } else {
    if (const ExportCall* init = find("init-plugin"))
      calls->push_back(*init);
    calls->push_back(*load);
    if (const ExportCall* enable = find("on-enable"))
      calls->push_back(*enable);
    if (const ExportCall* disable = find("on-disable"))
      calls->push_back(*disable);
  }
  return true;
}

} // namespace

bool prepare_component(const std::filesystem::path& path, PreparedComponent* out,
                       std::string* error) {
  const PackageReport gate = inspect_wasm(path);
  out->file = gate.module.file;
  out->sha256 = gate.module.sha256;
  out->size = gate.module.size;
  out->bytes.clear();
  out->calls.clear();
  out->bindings.clear();
  out->has_tick_callback = false;
  out->tick_callback = 0;
  out->tick_event_type = 40;
  out->tick_event_case = 40;
  out->has_task_callback = false;
  out->task_callback = 0;
  out->task_callback_status = false;
  out->task_callback_async = false;
  if (!gate.ok) {
    *error = gate.error;
    return false;
  }
  if (gate.status != "component") {
    *error = "the module is not a component";
    return false;
  }

  std::vector<std::uint8_t> bytes;
  if (!read_file(gate.module.file, &bytes, error))
    return false;

  // A compiler-produced component is linked at its core ABI: it carries a
  // nested component graph and a WIT type section far wider than the flat
  // strict linker models. Recognise that shape before attempting a strict
  // parse, so this is a deliberate linker choice rather than a downgrade.
  ComponentScan scan;
  std::string scan_error;
  if (scan_component(bytes, &scan, &scan_error) &&
      (scan.instance_defs > 0 || (scan.wasi && scan.modules.size() > 1))) {
    std::string compiled_error;
    if (prepare_compiled_core(scan, out, &compiled_error))
      return true;
    *error = "the compiled-core component is not hostable: " + compiled_error;
    return false;
  }

  Shape shape;
  std::string shape_error;
  if (!read_shape(bytes, &shape, &shape_error)) {
    *error = shape_error;
    return false;
  }

  if (shape.nested_components > 0) {
    *error = "the component nests " + std::to_string(shape.nested_components) +
             " component(s); the hosted subset is one flat component";
    return false;
  }
  if (shape.starts > 0) {
    *error = "the component has a start definition; the hosted subset drives the "
             "lifecycle exports only";
    return false;
  }
  if (shape.values > 0) {
    *error = "the component defines " + std::to_string(shape.values) +
             " value(s); value definitions are not hosted";
    return false;
  }
  if (shape.instance_defs > 0) {
    *error = "the component defines " + std::to_string(shape.instance_defs) +
             " component instance(s); only core linking is hosted";
    return false;
  }
  if (shape.modules.size() != 1) {
    *error = "the component holds " + std::to_string(shape.modules.size()) +
             " core module(s); the hosted subset runs exactly one";
    return false;
  }

  CoreModuleInfo module;
  if (!read_core_module(shape.modules.front(), &module, error))
    return false;

  std::string link_error;
  if (!link_shape(shape, module, &out->calls, &out->bindings, &link_error)) {
    *error = link_error;
    return false;
  }

  out->bytes = std::move(shape.modules.front());
  return true;
}

RunReport run_component(const std::filesystem::path& path, const Limits& limits, Host& host) {
  const auto started = std::chrono::steady_clock::now();
  RunReport report;
  report.limits = limits;
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

  PreparedComponent prepared;
  std::string error;
  if (!prepare_component(path, &prepared, &error)) {
    report.file = prepared.file;
    report.sha256 = prepared.sha256;
    report.size = prepared.size;
    report.kind = "component";
    report.error = error;
    return report;
  }

  report = run_core_in_memory(prepared.bytes, prepared.file, prepared.calls, limits, host,
                              prepared.bindings);
  report.file = prepared.file;
  report.sha256 = prepared.sha256;
  report.size = prepared.size;
  report.kind = "component";
  return report;
}

} // namespace mc::wasm
