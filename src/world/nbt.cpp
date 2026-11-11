// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/nbt.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace mc::world::nbt {

namespace {

// Anything above this is a corrupt length claim (defense in depth; real chunk
// NBT payloads are ~10 KiB and section light arrays are 2 KiB).
constexpr size_t kMaxArrayBytes = 16 * 1024 * 1024;
constexpr size_t kMaxStringBytes = 1024 * 1024;
constexpr size_t kMaxItems = 4 * 1024 * 1024;
constexpr int kMaxNbtDepth = 32;

// Bounds-checked cursor over the input payload.
class Reader {
public:
  Reader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

  bool ok() const {
    return pos_ <= len_;
  }
  size_t remaining() const {
    return len_ - pos_;
  }

  bool u8(uint8_t* out) {
    if (pos_ >= len_)
      return false;
    *out = data_[pos_++];
    return true;
  }

  template <typename T> bool take(T* out) {
    if (remaining() < sizeof(T))
      return false;
    if constexpr (sizeof(T) == 1) {
      *out = data_[pos_];
    } else if constexpr (sizeof(T) == 2) {
      uint16_t v = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
      std::memcpy(out, &v, sizeof(v));
    } else if constexpr (sizeof(T) == 4) {
      uint32_t v = 0;
      for (int i = 0; i < 4; ++i)
        v = (v << 8) | data_[pos_ + static_cast<size_t>(i)];
      std::memcpy(out, &v, sizeof(v));
    } else if constexpr (sizeof(T) == 8) {
      uint64_t v = 0;
      for (int i = 0; i < 8; ++i)
        v = (v << 8) | data_[pos_ + static_cast<size_t>(i)];
      std::memcpy(out, &v, sizeof(v));
    }
    pos_ += sizeof(T);
    return true;
  }

  bool bytes(size_t n, const uint8_t** out) {
    if (remaining() < n)
      return false;
    *out = data_ + pos_;
    pos_ += n;
    return true;
  }

private:
  const uint8_t* data_;
  size_t len_;
  size_t pos_ = 0;
};

bool read_string(Reader& r, std::string* out) {
  uint16_t len = 0;
  if (!r.take(&len))
    return false;
  // NBT strings are u16 length-prefixed so kMaxStringBytes is moot here,
  // but keep the guard for future callers that pass a larger limit.
  (void)kMaxStringBytes;
  const uint8_t* p = nullptr;
  if (!r.bytes(len, &p))
    return false;
  out->assign(reinterpret_cast<const char*>(p), len);
  return true;
}

bool read_value(Reader& r, Tag tag, Value* out, int depth = 0) {
  out->type = tag;
  switch (tag) {
  case Tag::Byte: {
    uint8_t b = 0;
    if (!r.u8(&b))
      return false;
    out->i = static_cast<int8_t>(b);
    return true;
  }
  case Tag::Short: {
    int16_t v = 0;
    if (!r.take(&v))
      return false;
    out->i = v;
    return true;
  }
  case Tag::Int: {
    int32_t v = 0;
    if (!r.take(&v))
      return false;
    out->i = v;
    return true;
  }
  case Tag::Long: {
    int64_t v = 0;
    if (!r.take(&v))
      return false;
    out->i = v;
    return true;
  }
  case Tag::Float: {
    uint32_t bits = 0;
    if (!r.take(&bits))
      return false;
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    out->d = f;
    return true;
  }
  case Tag::Double: {
    uint64_t bits = 0;
    if (!r.take(&bits))
      return false;
    double d = 0;
    std::memcpy(&d, &bits, sizeof(d));
    out->d = d;
    return true;
  }
  case Tag::ByteArray: {
    int32_t n = 0;
    if (!r.take(&n) || n < 0)
      return false;
    const size_t count = static_cast<size_t>(n);
    if (count > kMaxArrayBytes)
      return false;
    const uint8_t* p = nullptr;
    if (!r.bytes(count, &p))
      return false;
    out->bytes.assign(p, p + count);
    return true;
  }
  case Tag::IntArray: {
    int32_t n = 0;
    if (!r.take(&n) || n < 0)
      return false;
    const size_t count = static_cast<size_t>(n);
    if (count > kMaxItems || count > r.remaining() / 4)
      return false;
    out->ints.resize(count);
    for (auto& v : out->ints) {
      int32_t x = 0;
      if (!r.take(&x))
        return false;
      v = x;
    }
    return true;
  }
  case Tag::LongArray: {
    int32_t n = 0;
    if (!r.take(&n) || n < 0)
      return false;
    const size_t count = static_cast<size_t>(n);
    if (count > kMaxItems || count > r.remaining() / 8)
      return false;
    out->longs.resize(count);
    for (auto& v : out->longs) {
      int64_t x = 0;
      if (!r.take(&x))
        return false;
      v = x;
    }
    return true;
  }
  case Tag::String: {
    return read_string(r, &out->s);
  }
  case Tag::List: {
    if (depth >= kMaxNbtDepth)
      return false;
    uint8_t elem = 0;
    int32_t n = 0;
    if (!r.u8(&elem) || !r.take(&n) || n < 0)
      return false;
    const size_t count = static_cast<size_t>(n);
    if (count > kMaxItems)
      return false;
    if (elem == 0 && count != 0)
      return false;
    out->list_type = static_cast<Tag>(elem);
    if (elem != 0 && count > r.remaining())
      return false;
    // A declared count is a *claim*: it is bounded by the remaining input, but
    // a 16 MiB payload can still claim four million items, and reserving them
    // up front would hand a corrupt file a ~400 MiB allocation before a single
    // item was read. Reserve a prefix and let the vector grow on the items
    // that actually parse; the cap bounds the guess, never the answer.
    out->items.reserve(std::min<size_t>(count, 1024));
    for (size_t i = 0; i < count; ++i) {
      Value item;
      if (!read_value(r, static_cast<Tag>(elem), &item, depth + 1))
        return false;
      out->items.push_back(std::move(item));
    }
    return true;
  }
  case Tag::Compound: {
    if (depth >= kMaxNbtDepth)
      return false;
    // Every compound a chunk record carries (the root, each section, each
    // palette entry) has several fields, and the vector grew one at a time.
    // Eight is the usual shape and the reserve is dropped as soon as the
    // compound is stored, so a larger one costs one extra growth at most.
    out->fields.reserve(8);
    for (;;) {
      uint8_t t = 0;
      if (!r.u8(&t))
        return false;
      if (t == 0)
        return true;
      if (t > static_cast<uint8_t>(Tag::LongArray))
        return false;
      std::string name;
      if (!read_string(r, &name))
        return false;
      Value field;
      if (!read_value(r, static_cast<Tag>(t), &field, depth + 1))
        return false;
      out->fields.emplace_back(std::move(name), std::move(field));
    }
  }
  case Tag::End:
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Writer (Milestone E)
// ---------------------------------------------------------------------------

class Writer {
public:
  std::vector<uint8_t> buf;

  void u8(uint8_t v) {
    buf.push_back(v);
  }
  void be16(uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
  }
  void be32(uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 24));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
  }
  void be64(uint64_t v) {
    for (int i = 7; i >= 0; --i)
      buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
  }
  void str(const std::string& s) {
    if (s.size() > 65535) {
      // NBT strings are u16 length-prefixed; truncate to avoid silent wrap.
      // Callers (chunk serializer) never hit this; it guards fuzz input.
      be16(65535);
      buf.insert(buf.end(), s.begin(), s.begin() + 65535);
      return;
    }
    be16(static_cast<uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
  }
  void write_value_payload(const Value& v);
  void write_named(const std::string& name, const Value& v);
};

void Writer::write_value_payload(const Value& v) {
  switch (v.type) {
  case Tag::End:
    break;
  case Tag::Byte:
    u8(static_cast<uint8_t>(static_cast<int8_t>(v.i)));
    break;
  case Tag::Short:
    be16(static_cast<uint16_t>(static_cast<int16_t>(v.i)));
    break;
  case Tag::Int:
    be32(static_cast<uint32_t>(static_cast<int32_t>(v.i)));
    break;
  case Tag::Long:
    be64(static_cast<uint64_t>(v.i));
    break;
  case Tag::Float: {
    float f = static_cast<float>(v.d);
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    be32(bits);
    break;
  }
  case Tag::Double: {
    double d = v.d;
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    be64(bits);
    break;
  }
  case Tag::ByteArray:
    be32(static_cast<uint32_t>(v.bytes.size()));
    for (int8_t b : v.bytes)
      u8(static_cast<uint8_t>(b));
    break;
  case Tag::String:
    str(v.s);
    break;
  case Tag::List:
    u8(static_cast<uint8_t>(v.list_type));
    be32(static_cast<uint32_t>(v.items.size()));
    for (const auto& it : v.items)
      write_value_payload(it);
    break;
  case Tag::Compound:
    for (const auto& [name, field] : v.fields)
      write_named(name, field);
    u8(0); // End
    break;
  case Tag::IntArray:
    be32(static_cast<uint32_t>(v.ints.size()));
    for (int32_t x : v.ints)
      be32(static_cast<uint32_t>(x));
    break;
  case Tag::LongArray:
    be32(static_cast<uint32_t>(v.longs.size()));
    for (int64_t x : v.longs)
      be64(static_cast<uint64_t>(x));
    break;
  }
}

void Writer::write_named(const std::string& name, const Value& v) {
  u8(static_cast<uint8_t>(v.type));
  str(name);
  write_value_payload(v);
}

} // namespace

const Value* child(const Value& v, std::string_view key) {
  if (v.type != Tag::Compound)
    return nullptr;
  for (const auto& [name, field] : v.fields) {
    if (name == key)
      return &field;
  }
  return nullptr;
}

std::optional<int64_t> as_int(const Value& v) {
  switch (v.type) {
  case Tag::Byte:
  case Tag::Short:
  case Tag::Int:
  case Tag::Long:
    return v.i;
  default:
    return std::nullopt;
  }
}

std::optional<double> as_float(const Value& v) {
  if (v.type == Tag::Float || v.type == Tag::Double)
    return v.d;
  return std::nullopt;
}

std::optional<std::string_view> as_string(const Value& v) {
  if (v.type != Tag::String)
    return std::nullopt;
  return std::string_view(v.s);
}

const std::vector<int8_t>* as_bytes(const Value& v) {
  return v.type == Tag::ByteArray ? &v.bytes : nullptr;
}

const std::vector<int32_t>* as_ints(const Value& v) {
  return v.type == Tag::IntArray ? &v.ints : nullptr;
}

const std::vector<int64_t>* as_longs(const Value& v) {
  return v.type == Tag::LongArray ? &v.longs : nullptr;
}

std::optional<Value> parse(const uint8_t* data, size_t len) {
  if (data == nullptr || len < 2)
    return std::nullopt;
  Reader r(data, len);
  uint8_t t = 0;
  if (!r.u8(&t) || t != static_cast<uint8_t>(Tag::Compound))
    return std::nullopt;
  std::string root_name;
  if (!read_string(r, &root_name))
    return std::nullopt;
  Value root;
  if (!read_value(r, Tag::Compound, &root))
    return std::nullopt;
  if (root.type != Tag::Compound)
    return std::nullopt;
  return root;
}

std::vector<uint8_t> serialize(const Value& root, const std::string& root_name) {
  Writer w;
  w.u8(static_cast<uint8_t>(Tag::Compound));
  w.str(root_name);
  w.write_value_payload(root);
  return std::move(w.buf);
}

Value make_byte(int8_t v) {
  Value x;
  x.type = Tag::Byte;
  x.i = v;
  return x;
}
Value make_short(int16_t v) {
  Value x;
  x.type = Tag::Short;
  x.i = v;
  return x;
}
Value make_int(int32_t v) {
  Value x;
  x.type = Tag::Int;
  x.i = v;
  return x;
}
Value make_long(int64_t v) {
  Value x;
  x.type = Tag::Long;
  x.i = v;
  return x;
}
Value make_float(float v) {
  Value x;
  x.type = Tag::Float;
  x.d = v;
  return x;
}
Value make_double(double v) {
  Value x;
  x.type = Tag::Double;
  x.d = v;
  return x;
}
Value make_string(std::string s) {
  Value x;
  x.type = Tag::String;
  x.s = std::move(s);
  return x;
}
Value make_byte_array(std::vector<int8_t> b) {
  Value x;
  x.type = Tag::ByteArray;
  x.bytes = std::move(b);
  return x;
}
Value make_byte_array_u8(const std::vector<uint8_t>& b) {
  return make_byte_array_u8(b.data(), b.size());
}
Value make_byte_array_u8(const uint8_t* data, std::size_t n) {
  Value x;
  x.type = Tag::ByteArray;
  x.bytes.assign(data, data + n);
  return x;
}
Value make_int_array(std::vector<int32_t> v) {
  Value x;
  x.type = Tag::IntArray;
  x.ints = std::move(v);
  return x;
}
Value make_long_array(std::vector<int64_t> v) {
  Value x;
  x.type = Tag::LongArray;
  x.longs = std::move(v);
  return x;
}
Value make_list(Tag elem_type, std::vector<Value> items) {
  Value x;
  x.type = Tag::List;
  x.list_type = elem_type;
  x.items = std::move(items);
  return x;
}
Value make_compound(std::vector<std::pair<std::string, Value>> fields) {
  Value x;
  x.type = Tag::Compound;
  x.fields = std::move(fields);
  return x;
}

} // namespace mc::world::nbt
