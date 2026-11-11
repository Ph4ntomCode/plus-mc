// SPDX-License-Identifier: GPL-3.0-or-later
//
// The converter reports are JSON objects with no third-party writer. These two
// helpers are the whole of it: quote a string (including control characters)
// and print a bounded list. They are shared by the analyzer and the mapper so
// the two documents cannot drift in escaping.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace mc::converter::detail {

inline std::string json_quote(const std::string& value) {
  std::string out = "\"";
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        static constexpr char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0f]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  out += "\"";
  return out;
}

inline void json_list(std::string* out, const std::vector<std::string>& values,
                      std::size_t cap = 200) {
  *out += "[";
  const std::size_t shown = std::min(values.size(), cap);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i != 0)
      *out += ",";
    *out += json_quote(values[i]);
  }
  *out += "]";
}

} // namespace mc::converter::detail
