// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/hosting.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

#include "core/log.h"

namespace mc::server {

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
    ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
    --e;
  return s.substr(b, e - b);
}

std::string lower(std::string s) {
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// java.util.Properties escapes ':', '=' and backslashes in values.
std::string unescape_properties(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char next = s[i + 1];
      if (next == ':' || next == '=' || next == '\\' || next == '#') {
        out.push_back(next);
        ++i;
        continue;
      }
      if (next == 'n') {
        out.push_back('\n');
        ++i;
        continue;
      }
      if (next == 't') {
        out.push_back('\t');
        ++i;
        continue;
      }
      if (next == 'u' && i + 5 < s.size()) {
        // \uXXXX — decode into UTF-8 for the (ASCII) range we care about.
        unsigned code = 0;
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
          const char h = s[i + 2 + static_cast<size_t>(k)];
          code <<= 4;
          if (h >= '0' && h <= '9')
            code |= static_cast<unsigned>(h - '0');
          else if (h >= 'a' && h <= 'f')
            code |= static_cast<unsigned>(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F')
            code |= static_cast<unsigned>(h - 'A' + 10);
          else {
            ok = false;
            break;
          }
        }
        if (ok) {
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
          i += 5;
          continue;
        }
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

// Minimal scan for the string values of a flat JSON array of objects. A full
// JSON parser arrives with the registry loader; the player-list files are
// machine-written and flat, and every miss degrades to "no entry".
std::vector<PlayerListEntry> parse_player_list(const std::string& text) {
  std::vector<PlayerListEntry> out;
  size_t pos = 0;
  while (true) {
    const size_t obj = text.find('{', pos);
    if (obj == std::string::npos)
      break;
    const size_t end = text.find('}', obj);
    if (end == std::string::npos)
      break;
    const std::string body = text.substr(obj, end - obj);
    pos = end + 1;
    PlayerListEntry entry;
    size_t scan = 0;
    while (scan < body.size()) {
      const size_t key_open = body.find('"', scan);
      if (key_open == std::string::npos)
        break;
      const size_t key_close = body.find('"', key_open + 1);
      if (key_close == std::string::npos)
        break;
      const std::string key = body.substr(key_open + 1, key_close - key_open - 1);
      const size_t colon = body.find(':', key_close);
      if (colon == std::string::npos)
        break;
      // The value is a string when the first non-whitespace char after the
      // colon is a quote (the shape both hand-written and vanilla-written
      // player-list files use: `"name": "Notch"`), else a bare number.
      const size_t value_start = body.find_first_not_of(" \t\r\n", colon + 1);
      if (value_start != std::string::npos && body[value_start] == '"') {
        const size_t value_close = body.find('"', value_start + 1);
        if (value_close == std::string::npos)
          break;
        const std::string value = body.substr(value_start + 1, value_close - value_start - 1);
        if (key == "name")
          entry.name = value;
        else if (key == "uuid")
          entry.uuid = value;
        scan = value_close + 1;
      } else if (value_start != std::string::npos) {
        const size_t number_end = body.find_first_of(",}", value_start);
        const std::string value =
            body.substr(value_start, number_end == std::string::npos ? std::string::npos
                                                                     : number_end - value_start);
        if (key == "level")
          entry.level = std::atoi(value.c_str());
        scan = number_end == std::string::npos ? body.size() : number_end;
      } else {
        break;
      }
    }
    if (!entry.name.empty() || !entry.uuid.empty())
      out.push_back(std::move(entry));
  }
  return out;
}

} // namespace

ServerProperties ServerProperties::load(const fs::path& dir) {
  ServerProperties out;
  const std::string text = read_file(dir / "server.properties");
  if (text.empty())
    return out;
  out.present_ = true;
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == '!')
      continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      out.values_.emplace(trimmed, std::string());
      continue;
    }
    out.values_.emplace(trim(trimmed.substr(0, eq)),
                        unescape_properties(trim(trimmed.substr(eq + 1))));
  }
  return out;
}

std::optional<std::string> ServerProperties::get(const std::string& key) const {
  const auto it = values_.find(key);
  if (it == values_.end())
    return std::nullopt;
  return it->second;
}

bool ServerProperties::get_bool(const std::string& key, bool fallback) const {
  const auto it = values_.find(key);
  if (it == values_.end())
    return fallback;
  const std::string v = lower(trim(it->second));
  if (v == "true" || v == "1" || v == "yes")
    return true;
  if (v == "false" || v == "0" || v == "no")
    return false;
  return fallback;
}

int ServerProperties::get_int(const std::string& key, int fallback) const {
  const auto it = values_.find(key);
  if (it == values_.end())
    return fallback;
  const std::string v = trim(it->second);
  char* end = nullptr;
  const long parsed = std::strtol(v.c_str(), &end, 10);
  if (end == v.c_str() || *end != '\0')
    return fallback;
  return static_cast<int>(parsed);
}

std::vector<std::string> ServerProperties::get_list(const std::string& key) const {
  std::vector<std::string> out;
  const auto it = values_.find(key);
  if (it == values_.end())
    return out;
  size_t pos = 0;
  while (pos <= it->second.size()) {
    const size_t comma = it->second.find(',', pos);
    const std::string part = trim(comma == std::string::npos ? it->second.substr(pos)
                                                             : it->second.substr(pos, comma - pos));
    if (!part.empty())
      out.push_back(part);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return out;
}

EulaState read_eula(const fs::path& dir) {
  std::ifstream in(dir / "eula.txt", std::ios::binary);
  if (!in)
    return EulaState::Missing;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::istringstream stream(text);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#')
      continue;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos)
      continue;
    if (lower(trim(trimmed.substr(0, eq))) != "eula")
      continue;
    const std::string value = lower(trim(trimmed.substr(eq + 1)));
    return value == "true" ? EulaState::Accepted : EulaState::Rejected;
  }
  return EulaState::Rejected;
}

bool write_eula_template(const fs::path& dir) {
  const fs::path path = dir / "eula.txt";
  std::error_code ec;
  if (fs::exists(path, ec))
    return false;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << "#By changing the setting below to TRUE you are indicating your agreement to our EULA "
         "(https://aka.ms/MinecraftEULA).\n"
         "eula=false\n";
  return static_cast<bool>(out);
}

std::vector<PlayerListEntry> load_player_list(const fs::path& path) {
  const std::string text = read_file(path);
  if (text.empty())
    return {};
  return parse_player_list(text);
}

bool list_contains_name(const std::vector<PlayerListEntry>& list, const std::string& name) {
  const std::string wanted = lower(name);
  for (const PlayerListEntry& entry : list) {
    if (!entry.name.empty() && lower(entry.name) == wanted)
      return true;
  }
  return false;
}

std::string format_uuid(const uint8_t uuid[16]) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10)
      out.push_back('-');
    out.push_back(kHex[uuid[i] >> 4]);
    out.push_back(kHex[uuid[i] & 0x0F]);
  }
  return out;
}

} // namespace mc::server
