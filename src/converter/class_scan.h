// SPDX-License-Identifier: GPL-3.0-or-later
//
// A bounded JVM class-file reader for the +MC Converter (Phase 7, slice 1).
// It reads the constant pool and the class header only: enough to name the
// class, its superclass and every class/method/field it references, without a
// JVM, a bytecode interpreter or decompilation. A malformed file is an error
// naming the first structural problem, never a crash or an unbounded read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mc::converter {

struct ClassScan {
  std::string name; // Internal form, e.g. `com/example/Plugin`.
  std::string super_name;
  uint16_t major = 0;
  uint16_t minor = 0;
  std::vector<std::string> interfaces;
  std::vector<std::string> class_refs;
  // `owner#name descriptor`, deduplicated by the caller.
  std::vector<std::string> method_refs;
  std::vector<std::string> field_refs;
  std::vector<std::string> strings;
  std::size_t method_count = 0;
};

// `size` is capped by the caller (the JAR reader's per-entry limit). Returns
// false and a one-line reason when the file is not a structurally valid class
// file. `out` is only meaningful when the call returns true.
bool parse_class(const uint8_t* data, std::size_t size, ClassScan* out, std::string* error);

} // namespace mc::converter
