// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hosting-panel compatibility (Phase 2 milestone F): the four files every
// Minecraft host writes and expects the server to honour.
//
//   server.properties  key=value lines (# comments; `\:` and `\=` escapes as
//                      vanilla's java.util.Properties writer emits them)
//   eula.txt           `eula=true` — the server refuses to start otherwise
//   whitelist.json     [{"uuid": "...", "name": "..."}]
//   ops.json           the same, plus "level" (op permission level)
//
// Parsing is deliberately tolerant: a malformed file must never crash the
// server, it degrades to "no entries" (and the caller logs it) — the same
// posture as the world-format readers (docs/PHASE2.md).
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mc::server {

// A parsed server.properties. Unknown keys are kept (so config-managing
// plugins can read them); known keys get typed accessors with vanilla
// defaults at the call site.
class ServerProperties {
public:
  // Reads <dir>/server.properties. A missing file leaves present() == false
  // and an empty map (the caller then uses its own defaults).
  static ServerProperties load(const std::filesystem::path& dir);

  bool present() const {
    return present_;
  }

  std::optional<std::string> get(const std::string& key) const;
  bool get_bool(const std::string& key, bool fallback) const;
  int get_int(const std::string& key, int fallback) const;
  // Comma-separated list, trimmed, empties dropped.
  std::vector<std::string> get_list(const std::string& key) const;

  const std::unordered_map<std::string, std::string>& values() const {
    return values_;
  }

private:
  bool present_ = false;
  std::unordered_map<std::string, std::string> values_;
};

enum class EulaState {
  Missing,  // no eula.txt
  Accepted, // eula=true
  Rejected, // eula=false (or unparsable)
};

// Reads <dir>/eula.txt. The file is *not* created by read_eula; callers that
// enforce the gate bootstrap one with write_eula_template (vanilla writes
// eula=false on first start, then refuses to run).
EulaState read_eula(const std::filesystem::path& dir);

// Writes a fresh <dir>/eula.txt with `eula=false` and the comment vanilla
// emits. Never overwrites an existing file. Returns false when unwritable.
bool write_eula_template(const std::filesystem::path& dir);

// One entry of whitelist.json / ops.json.
struct PlayerListEntry {
  std::string name; // "Notch"
  std::string uuid; // dashed or undashed; empty when the file omits it
  int level = 0;    // ops.json "level" (whitelist entries are always 0)
};

// Reads a whitelist.json / ops.json style file. Missing or malformed files
// yield an empty list.
std::vector<PlayerListEntry> load_player_list(const std::filesystem::path& path);

// Case-insensitive name membership over a player list (offline mode has no
// authoritative uuid to match on, so names are the practical key).
bool list_contains_name(const std::vector<PlayerListEntry>& list, const std::string& name);

// Formats a 16-byte uuid as the dashed string a player list uses.
std::string format_uuid(const uint8_t uuid[16]);

} // namespace mc::server
