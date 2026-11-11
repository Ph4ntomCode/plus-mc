// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal JSON string helpers shared across server components. Full JSON
// parsing arrives with the registry loader (Phase 2); these only need to
// escape strings and extract a handful of scalar fields from config files.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace mc::core {

// JSON-string escaping: quotes, backslashes, and control characters as \u00XX.
// UTF-8 continuation bytes (>= 0x80) pass through untouched.
inline std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  static constexpr char kHex[] = "0123456789abcdef";
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      out += "\\u00";
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

// Extracts a top-level integer value from a minimal JSON string.
inline std::optional<int64_t> json_int_field(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos)
    return std::nullopt;
  p = json.find(':', p + needle.size());
  if (p == std::string::npos)
    return std::nullopt;
  ++p;
  while (p < json.size() &&
         (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r'))
    ++p;
  bool neg = false;
  if (p < json.size() && json[p] == '-') {
    neg = true;
    ++p;
  }
  int64_t value = 0;
  bool any = false;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
    value = value * 10 + (json[p] - '0');
    any = true;
    ++p;
  }
  if (!any)
    return std::nullopt;
  return neg ? -value : value;
}

// Extracts a top-level string value from a minimal JSON string.
inline std::optional<std::string> json_string_field(const std::string& json,
                                                    const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos)
    return std::nullopt;
  p = json.find(':', p + needle.size());
  if (p == std::string::npos)
    return std::nullopt;
  p = json.find('"', p + 1);
  if (p == std::string::npos)
    return std::nullopt;
  size_t end = json.find('"', p + 1);
  if (end == std::string::npos)
    return std::nullopt;
  return json.substr(p + 1, end - p - 1);
}

} // namespace mc::core
