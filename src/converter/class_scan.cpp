// SPDX-License-Identifier: GPL-3.0-or-later

#include "converter/class_scan.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mc::converter {
namespace {

constexpr uint16_t kMinMajor = 45;
constexpr uint16_t kMaxMajor = 70;
constexpr std::size_t kMaxNameLength = 4096;

struct Reader {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  std::size_t pos = 0;

  bool take(std::size_t count) {
    if (count > size - pos) {
      return false;
    }
    pos += count;
    return true;
  }
  bool u1(uint8_t* out) {
    if (pos >= size)
      return false;
    *out = data[pos++];
    return true;
  }
  bool u2(uint16_t* out) {
    if (size - pos < 2)
      return false;
    *out = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) |
                                 static_cast<uint16_t>(data[pos + 1]));
    pos += 2;
    return true;
  }
  bool u4(uint32_t* out) {
    if (size - pos < 4)
      return false;
    *out = (static_cast<uint32_t>(data[pos]) << 24) | (static_cast<uint32_t>(data[pos + 1]) << 16) |
           (static_cast<uint32_t>(data[pos + 2]) << 8) | static_cast<uint32_t>(data[pos + 3]);
    pos += 4;
    return true;
  }
  bool bytes(std::size_t count, std::string* out) {
    if (count > size - pos)
      return false;
    out->assign(reinterpret_cast<const char*>(data + pos), count);
    pos += count;
    return true;
  }
};

struct Constant {
  uint8_t tag = 0;
  uint16_t first = 0;
  uint16_t second = 0;
  std::string text;
};

bool utf8_at(const std::vector<Constant>& pool, uint16_t index, const std::string** out,
             std::string* error) {
  if (index == 0 || index >= pool.size() || pool[index].tag != 1) {
    *error = "class file names a constant that is not a UTF-8 string";
    return false;
  }
  *out = &pool[index].text;
  return true;
}

bool class_name_at(const std::vector<Constant>& pool, uint16_t index, const std::string** out,
                   std::string* error) {
  if (index == 0 || index >= pool.size() || pool[index].tag != 7) {
    *error = "class file names a class constant that is not a class";
    return false;
  }
  return utf8_at(pool, pool[index].first, out, error);
}

bool skip_attributes(Reader* reader, uint16_t count, std::string* error) {
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t name = 0;
    uint32_t length = 0;
    if (!reader->u2(&name) || !reader->u4(&length)) {
      *error = "truncated attribute table";
      return false;
    }
    if (!reader->take(length)) {
      *error = "attribute runs past the end of the class file";
      return false;
    }
  }
  return true;
}

std::string join(const std::string& owner, const std::string& name, const std::string& descriptor) {
  return owner + "#" + name + " " + descriptor;
}

} // namespace

bool parse_class(const uint8_t* data, std::size_t size, ClassScan* out, std::string* error) {
  *out = ClassScan{};
  Reader reader{data, size, 0};

  uint32_t magic = 0;
  uint16_t minor = 0;
  uint16_t major = 0;
  uint16_t pool_count = 0;
  if (!reader.u4(&magic) || !reader.u2(&minor) || !reader.u2(&major) || !reader.u2(&pool_count)) {
    *error = "class file is shorter than its header";
    return false;
  }
  if (magic != 0xcafebabeu) {
    *error = "class file does not start with CAFEBABE";
    return false;
  }
  if (major < kMinMajor || major > kMaxMajor) {
    *error =
        "unsupported class-file version " + std::to_string(major) + "." + std::to_string(minor);
    return false;
  }
  if (pool_count == 0) {
    *error = "constant pool is empty";
    return false;
  }

  std::vector<Constant> pool(pool_count);
  for (uint16_t i = 1; i < pool_count; ++i) {
    Constant& constant = pool[i];
    if (!reader.u1(&constant.tag)) {
      *error = "truncated constant pool";
      return false;
    }
    switch (constant.tag) {
    case 1: { // Utf8
      uint16_t length = 0;
      if (!reader.u2(&length)) {
        *error = "truncated UTF-8 constant";
        return false;
      }
      if (length > kMaxNameLength) {
        *error = "UTF-8 constant is longer than the analyzer's limit";
        return false;
      }
      if (!reader.bytes(length, &constant.text)) {
        *error = "UTF-8 constant runs past the end of the class file";
        return false;
      }
      break;
    }
    case 3: // Integer
    case 4: // Float
      if (!reader.take(4)) {
        *error = "truncated numeric constant";
        return false;
      }
      break;
    case 5: // Long
    case 6: // Double
      if (!reader.take(8)) {
        *error = "truncated wide constant";
        return false;
      }
      if (i + 1 >= pool_count) {
        *error = "wide constant has no second slot";
        return false;
      }
      ++i; // Long and Double take two constant-pool slots.
      break;
    case 7:  // Class
    case 8:  // String
    case 16: // MethodType
    case 19: // Module
    case 20: // Package
      if (!reader.u2(&constant.first)) {
        *error = "truncated constant pool entry";
        return false;
      }
      break;
    case 9:  // Fieldref
    case 10: // Methodref
    case 11: // InterfaceMethodref
    case 12: // NameAndType
    case 17: // Dynamic
    case 18: // InvokeDynamic
      if (!reader.u2(&constant.first) || !reader.u2(&constant.second)) {
        *error = "truncated constant pool entry";
        return false;
      }
      break;
    case 15: { // MethodHandle
      uint8_t kind = 0;
      if (!reader.u1(&kind) || !reader.u2(&constant.first)) {
        *error = "truncated method handle";
        return false;
      }
      break;
    }
    default:
      *error = "unknown constant-pool tag " + std::to_string(constant.tag);
      return false;
    }
  }

  uint16_t access_flags = 0;
  uint16_t this_class = 0;
  uint16_t super_class = 0;
  uint16_t interface_count = 0;
  if (!reader.u2(&access_flags) || !reader.u2(&this_class) || !reader.u2(&super_class) ||
      !reader.u2(&interface_count)) {
    *error = "truncated class header";
    return false;
  }
  static_cast<void>(access_flags);

  const std::string* name = nullptr;
  if (!class_name_at(pool, this_class, &name, error))
    return false;
  out->name = *name;
  if (super_class != 0) {
    const std::string* super = nullptr;
    if (!class_name_at(pool, super_class, &super, error))
      return false;
    out->super_name = *super;
  }
  for (uint16_t i = 0; i < interface_count; ++i) {
    uint16_t index = 0;
    const std::string* interface_name = nullptr;
    if (!reader.u2(&index) || !class_name_at(pool, index, &interface_name, error))
      return false;
    out->interfaces.push_back(*interface_name);
  }

  uint16_t field_count = 0;
  if (!reader.u2(&field_count)) {
    *error = "truncated field table";
    return false;
  }
  for (uint16_t i = 0; i < field_count; ++i) {
    uint16_t flags = 0;
    uint16_t field_name = 0;
    uint16_t descriptor = 0;
    uint16_t attributes = 0;
    if (!reader.u2(&flags) || !reader.u2(&field_name) || !reader.u2(&descriptor) ||
        !reader.u2(&attributes)) {
      *error = "truncated field entry";
      return false;
    }
    static_cast<void>(flags);
    static_cast<void>(field_name);
    static_cast<void>(descriptor);
    if (!skip_attributes(&reader, attributes, error))
      return false;
  }

  uint16_t method_count = 0;
  if (!reader.u2(&method_count)) {
    *error = "truncated method table";
    return false;
  }
  out->method_count = method_count;
  for (uint16_t i = 0; i < method_count; ++i) {
    uint16_t flags = 0;
    uint16_t method_name = 0;
    uint16_t descriptor = 0;
    uint16_t attributes = 0;
    if (!reader.u2(&flags) || !reader.u2(&method_name) || !reader.u2(&descriptor) ||
        !reader.u2(&attributes)) {
      *error = "truncated method entry";
      return false;
    }
    static_cast<void>(flags);
    static_cast<void>(method_name);
    static_cast<void>(descriptor);
    if (!skip_attributes(&reader, attributes, error))
      return false;
  }

  uint16_t class_attributes = 0;
  if (!reader.u2(&class_attributes) || !skip_attributes(&reader, class_attributes, error))
    return false;

  out->major = major;
  out->minor = minor;

  // The references are the constant pool's, not a bytecode walk: a class,
  // method or field name is in this list only because the compiler emitted a
  // constant for it, which is what a decompile-free scan can honestly say.
  for (uint16_t i = 1; i < pool_count; ++i) {
    const Constant& constant = pool[i];
    if (constant.tag == 7) {
      const std::string* ref = nullptr;
      if (!utf8_at(pool, constant.first, &ref, error))
        return false;
      if (!ref->empty() && ref->front() != '[')
        out->class_refs.push_back(*ref);
    } else if (constant.tag == 8) {
      const std::string* value = nullptr;
      if (!utf8_at(pool, constant.first, &value, error))
        return false;
      out->strings.push_back(*value);
    } else if (constant.tag == 9 || constant.tag == 10 || constant.tag == 11) {
      if (constant.second == 0 || constant.second >= pool.size() ||
          pool[constant.second].tag != 12) {
        *error = "member reference has no name-and-type";
        return false;
      }
      const Constant& name_and_type = pool[constant.second];
      const std::string* owner = nullptr;
      const std::string* member_name = nullptr;
      const std::string* descriptor = nullptr;
      if (!class_name_at(pool, constant.first, &owner, error) ||
          !utf8_at(pool, name_and_type.first, &member_name, error) ||
          !utf8_at(pool, name_and_type.second, &descriptor, error)) {
        return false;
      }
      if (constant.tag == 9)
        out->field_refs.push_back(join(*owner, *member_name, *descriptor));
      else
        out->method_refs.push_back(join(*owner, *member_name, *descriptor));
    }
  }

  const auto dedupe = [](std::vector<std::string>* values) {
    std::sort(values->begin(), values->end());
    values->erase(std::unique(values->begin(), values->end()), values->end());
  };
  dedupe(&out->class_refs);
  dedupe(&out->method_refs);
  dedupe(&out->field_refs);
  dedupe(&out->strings);
  return true;
}

} // namespace mc::converter
