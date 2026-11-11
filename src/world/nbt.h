// SPDX-License-Identifier: GPL-3.0-or-later
//
// Read-only NBT (Named Binary Tag) parser for world data (26.2 Anvil chunk
// NBT). NBT is big-endian throughout; strings are UTF-8 with a u16 length;
// lists carry a one-byte element type; compound fields are name/tag pairs
// terminated by a zero tag.
//
// The parser is hardened the same way as the protocol framing: every read is
// bounds-checked, allocation sizes are capped, and malformed input yields
// std::nullopt instead of undefined behavior. Parsing produces a lightweight
// tree that the Anvil chunk loader and the wire encoder query directly.
//
// Milestone E adds a symmetric writer: nbt::serialize() produces the binary
// form of a Value tree (root compound + name), suitable for region records
// (zlib) or level.dat (gzip). The writer mirrors the parser's big-endian
// layout exactly so a serialize → parse round-trip is identity.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc::world::nbt {

// Tag ids as they appear on the wire (NBT format is stable since forever).
enum class Tag : uint8_t {
  End = 0,
  Byte = 1,
  Short = 2,
  Int = 3,
  Long = 4,
  Float = 5,
  Double = 6,
  ByteArray = 7,
  String = 8,
  List = 9,
  Compound = 10,
  IntArray = 11,
  LongArray = 12,
};

// Parsed NBT value. Only the member matching `type` is meaningful; the others
// are left defaulted. Compounds keep insertion order (field order is
// semantically irrelevant to consumers but preserved for fidelity).
struct Value {
  Tag type = Tag::End;
  int64_t i = 0;                                     // Byte / Short / Int / Long
  double d = 0;                                      // Float / Double
  std::string s;                                     // String
  std::vector<int8_t> bytes;                         // ByteArray
  std::vector<int32_t> ints;                         // IntArray
  std::vector<int64_t> longs;                        // LongArray
  Tag list_type = Tag::End;                          // List (element type)
  std::vector<Value> items;                          // List elements
  std::vector<std::pair<std::string, Value>> fields; // Compound (ordered)

  // Declared and defaulted out of line. `fields` is a vector of a pair that is
  // itself incomplete while this class is being defined; libstdc++'s destructor
  // checks (std::is_destructible on the pair) accept that lazily, but clang
  // instantiates them in the complete-class context and rejects the pair as
  // incomplete. Defining the special members after the closing brace makes
  // every one of those instantiations happen with `Value` complete.
  Value();
  Value(const Value&);
  Value(Value&&) noexcept;
  Value& operator=(const Value&);
  Value& operator=(Value&&) noexcept;
  ~Value();
};

inline Value::Value() = default;
inline Value::Value(const Value&) = default;
inline Value::Value(Value&&) noexcept = default;
inline Value& Value::operator=(const Value&) = default;
inline Value& Value::operator=(Value&&) noexcept = default;
inline Value::~Value() = default;

// Returns the child compound field named `key`, or nullptr when `v` is not a
// compound or has no such field.
const Value* child(const Value& v, std::string_view key);

// Typed accessors. Each returns std::nullopt when `v` has the wrong type.
std::optional<int64_t> as_int(const Value& v);  // byte/short/int/long
std::optional<double> as_float(const Value& v); // float/double
std::optional<std::string_view> as_string(const Value& v);
const std::vector<int8_t>* as_bytes(const Value& v);  // ByteArray
const std::vector<int32_t>* as_ints(const Value& v);  // IntArray
const std::vector<int64_t>* as_longs(const Value& v); // LongArray

// Parses one named root compound from `data` (the entire payload, starting at
// the root's tag byte). Chunk NBT roots carry an empty name. Returns nullopt
// on truncation, an unknown tag, or an implausible length claim. The returned
// Value is the root compound itself.
std::optional<Value> parse(const uint8_t* data, size_t len);

// Serializes a root compound `root` (must be Tag::Compound) with name
// `root_name` (empty for chunk NBT) to the NBT binary format. The output is
// the exact bytes that parse() consumes (tag + name + payload).
std::vector<uint8_t> serialize(const Value& root, const std::string& root_name = "");

// Helpers to build Values concisely (all set `type` correctly).
Value make_byte(int8_t v);
Value make_short(int16_t v);
Value make_int(int32_t v);
Value make_long(int64_t v);
Value make_float(float v);
Value make_double(double v);
Value make_string(std::string s);
Value make_byte_array(std::vector<int8_t> b);
Value make_byte_array_u8(const std::vector<uint8_t>& b);
// The same array from a buffer: a section's light is a `LightArray`
// (`core/slab_pool.h`), which is not a `std::vector<uint8_t>` and should not be
// copied into one to be written back verbatim.
Value make_byte_array_u8(const uint8_t* data, std::size_t n);
Value make_int_array(std::vector<int32_t> v);
Value make_long_array(std::vector<int64_t> v);
Value make_list(Tag elem_type, std::vector<Value> items);
Value make_compound(std::vector<std::pair<std::string, Value>> fields);

} // namespace mc::world::nbt
