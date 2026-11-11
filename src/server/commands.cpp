// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/commands.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

namespace mc::server::cmd {

std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    if (i >= s.size())
      break;
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])))
      ++j;
    out.emplace_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

static std::string to_lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int parse_gamemode_token(const std::string& s) {
  const std::string t = to_lower(s);
  if (t == "0" || t == "s" || t == "survival")
    return 0;
  if (t == "1" || t == "c" || t == "creative")
    return 1;
  if (t == "2" || t == "a" || t == "adventure")
    return 2;
  if (t == "3" || t == "sp" || t == "spectator")
    return 3;
  return -1;
}

std::optional<double> parse_coord(const std::string& tok, double origin) {
  if (tok.empty())
    return std::nullopt;
  auto finite = [](double v) { return std::isfinite(v); };
  if (tok[0] == '~') {
    if (tok.size() == 1)
      return origin;
    std::string off = tok.substr(1);
    try {
      size_t pos = 0;
      double d = std::stod(off, &pos);
      if (pos != off.size() || !finite(d))
        return std::nullopt;
      double out = origin + d;
      if (!finite(out))
        return std::nullopt;
      return out;
    } catch (...) {
      return std::nullopt;
    }
  }
  try {
    size_t pos = 0;
    double d = std::stod(tok, &pos);
    if (pos != tok.size() || !finite(d))
      return std::nullopt;
    return d;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<float> parse_angle(const std::string& tok, float origin) {
  if (tok.empty())
    return std::nullopt;
  auto finite = [](float v) { return std::isfinite(v); };
  if (tok[0] == '~') {
    if (tok.size() == 1)
      return origin;
    std::string off = tok.substr(1);
    try {
      size_t pos = 0;
      float f = std::stof(off, &pos);
      if (pos != off.size() || !finite(f))
        return std::nullopt;
      float out = origin + f;
      if (!finite(out))
        return std::nullopt;
      return out;
    } catch (...) {
      return std::nullopt;
    }
  }
  try {
    size_t pos = 0;
    float f = std::stof(tok, &pos);
    if (pos != tok.size() || !finite(f))
      return std::nullopt;
    return f;
  } catch (...) {
    return std::nullopt;
  }
}

std::string normalize_id(const std::string& raw) {
  if (raw.find(':') != std::string::npos)
    return to_lower(raw);
  return "minecraft:" + to_lower(raw);
}

std::optional<int64_t> parse_time_value(const std::string& s) {
  std::string t = to_lower(s);
  if (t == "day")
    return 1000;
  if (t == "noon")
    return 6000;
  if (t == "midnight")
    return 18000;
  if (t == "night")
    return 13000;
  // Strip separators like "1_000" or "1,000": they must appear between
  // digits, not at edges, consecutively, or adjacent to a minus sign.
  if (!t.empty() && (t.front() == '_' || t.front() == ',' || t.back() == '_' || t.back() == ','))
    return std::nullopt;
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if ((t[i] == '_' || t[i] == ',') && (t[i + 1] == '_' || t[i + 1] == ','))
      return std::nullopt;
    if ((t[i] == '_' || t[i] == ',') && t[i + 1] == '-')
      return std::nullopt;
    if (t[i] == '-' && (t[i + 1] == '_' || t[i + 1] == ','))
      return std::nullopt;
  }
  std::string digits;
  digits.reserve(t.size());
  for (char c : t) {
    if (c == '_' || c == ',')
      continue;
    digits.push_back(c);
  }
  if (digits.empty())
    return std::nullopt;
  int64_t v = 0;
  auto res = std::from_chars(digits.c_str(), digits.c_str() + digits.size(), v);
  if (res.ec != std::errc{} || res.ptr != digits.c_str() + digits.size())
    return std::nullopt;
  return v;
}

int64_t time_marker_ticks(const std::string& marker) {
  auto v = parse_time_value(marker);
  return v ? *v : -1;
}

std::optional<int> parse_count(const std::string& s) {
  int v = 0;
  auto res = std::from_chars(s.c_str(), s.c_str() + s.size(), v);
  if (res.ec != std::errc{} || res.ptr != s.c_str() + s.size())
    return std::nullopt;
  if (v < 1 || v > 64)
    return std::nullopt;
  return v;
}

std::string lower_ascii(const std::string& s) {
  std::string out = s;
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool name_equals_ci(const std::string& a, const std::string& b) {
  return lower_ascii(a) == lower_ascii(b);
}

bool target_is_self(const std::string& target, const std::string& self_name) {
  const std::string lower = lower_ascii(target);
  return lower == "@s" || lower == "@p" || lower == lower_ascii(self_name);
}

} // namespace mc::server::cmd
