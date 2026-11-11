// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scoreboard's file codec. See world/scoreboard_save.h for the schema and
// the two documented differences from vanilla's `scoreboard.dat`; this file is
// the NBT conversion and the gzip + atomic temp+rename wrappers.
#include "world/scoreboard_save.h"

#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.h"

namespace mc::world {

namespace {

bool gzip_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  std::vector<uint8_t> buf(1 << 16);
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = deflate(&zs, Z_FINISH);
    if (rc == Z_STREAM_ERROR) {
      deflateEnd(&zs);
      return false;
    }
    const size_t produced = buf.size() - zs.avail_out;
    out->insert(out->end(), buf.data(), buf.data() + produced);
    if (rc == Z_STREAM_END)
      break;
  }
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

constexpr size_t kMaxDecompressedBytes = 16 * 1024 * 1024;

bool inflate_any(const uint8_t* in, size_t in_len, std::vector<uint8_t>* out) {
  out->clear();
  z_stream zs{};
  if (inflateInit2(&zs, 15 + 32) != Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in);
  zs.avail_in = static_cast<uInt>(in_len);
  std::vector<uint8_t> buf(1 << 16);
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    const size_t produced = buf.size() - zs.avail_out;
    if (out->size() + produced > kMaxDecompressedBytes) {
      inflateEnd(&zs);
      return false;
    }
    out->insert(out->end(), buf.data(), buf.data() + produced);
  }
  inflateEnd(&zs);
  return rc == Z_STREAM_END;
}

bool list_field(const nbt::Value& node, const char* key, nbt::Tag type, const nbt::Value** out) {
  const nbt::Value* v = nbt::child(node, key);
  *out = v;
  if (v == nullptr)
    return true;
  return v->type == nbt::Tag::List && v->list_type == type;
}

// `DisplaySlot`'s file key: vanilla's own names for the three screen slots and
// `sidebar.team.<colour>` for the sixteen team-colour sidebars.
std::string display_slot_key(uint8_t slot) {
  switch (slot) {
  case kDisplayList:
    return "list";
  case kDisplaySidebar:
    return "sidebar";
  case kDisplayBelowName:
    return "belowName";
  default:
    break;
  }
  const int color = static_cast<int>(slot) - kDisplayTeamColorFirst;
  if (color < 0 || color >= kTeamColorCount)
    return {};
  return std::string("sidebar.team.") + team_color_name(static_cast<uint8_t>(color));
}

// The inverse; 100 for a key this build does not know.
int display_slot_from_key(std::string_view key) {
  if (key == "list")
    return kDisplayList;
  if (key == "sidebar")
    return kDisplaySidebar;
  if (key == "belowName")
    return kDisplayBelowName;
  constexpr std::string_view kTeamPrefix = "sidebar.team.";
  if (key.size() > kTeamPrefix.size() && key.substr(0, kTeamPrefix.size()) == kTeamPrefix) {
    const int color = team_color_id(key.substr(kTeamPrefix.size()));
    if (color >= 0)
      return kDisplayTeamColorFirst + color;
  }
  return -1;
}

const char* render_type_name(uint8_t type) {
  return type == kRenderHearts ? "hearts" : "integer";
}

uint8_t render_type_from_name(std::string_view name) {
  return name == "hearts" ? kRenderHearts : kRenderInteger;
}

// A byte/bool field, with vanilla's default when the field is absent.
int int_field(const nbt::Value& node, const char* key, int fallback) {
  const nbt::Value* v = nbt::child(node, key);
  if (v == nullptr)
    return fallback;
  const std::optional<int64_t> value = nbt::as_int(*v);
  return value ? static_cast<int>(*value) : fallback;
}

std::string string_field(const nbt::Value& node, const char* key, std::string fallback) {
  const nbt::Value* v = nbt::child(node, key);
  if (v == nullptr)
    return fallback;
  const std::optional<std::string_view> value = nbt::as_string(*v);
  return value ? std::string(*value) : std::move(fallback);
}

} // namespace

nbt::Value scoreboard_state_to_nbt(const Scoreboard& scoreboard) {
  using nbt::Tag;
  using nbt::Value;

  std::vector<Value> objective_rows;
  objective_rows.reserve(scoreboard.objectives().size());
  for (const auto& [name, objective] : scoreboard.objectives()) {
    objective_rows.push_back(nbt::make_compound({
        {"Name", nbt::make_string(name)},
        {"CriteriaName", nbt::make_string(objective.criteria.name)},
        {"DisplayName", nbt::make_string(objective.display_name)},
        {"RenderType", nbt::make_string(render_type_name(objective.render_type))},
        {"DisplayAutoUpdate", nbt::make_byte(objective.display_auto_update ? 1 : 0)},
    }));
  }

  // `Scoreboard`'s scores are objective -> owner. Written in that map's own
  // order, which is likewise the order a reload restores them in.
  std::vector<Value> score_rows;
  for (const auto& [name, objective] : scoreboard.objectives()) {
    for (const auto& [owner, score] : scoreboard.scores(name)) {
      score_rows.push_back(nbt::make_compound({
          {"Name", nbt::make_string(owner)},
          {"Objective", nbt::make_string(name)},
          {"Score", nbt::make_int(score.value)},
          {"Locked", nbt::make_byte(score.locked ? 1 : 0)},
      }));
    }
  }

  std::vector<std::pair<std::string, Value>> display_fields;
  for (uint8_t slot = 0; slot < kDisplaySlotCount; ++slot) {
    const std::string* shown = scoreboard.display_objective_name(slot);
    if (shown == nullptr)
      continue;
    const std::string key = display_slot_key(slot);
    if (key.empty())
      continue;
    display_fields.emplace_back(key, nbt::make_string(*shown));
  }

  std::vector<Value> team_rows;
  team_rows.reserve(scoreboard.teams().size());
  for (const auto& [name, team] : scoreboard.teams()) {
    std::vector<Value> players;
    players.reserve(team.players.size());
    for (const std::string& player : team.players)
      players.push_back(nbt::make_string(player));
    team_rows.push_back(nbt::make_compound({
        {"Name", nbt::make_string(name)},
        {"DisplayName", nbt::make_string(team.display_name)},
        {"Color", nbt::make_int(team.has_color ? static_cast<int32_t>(team.color) : -1)},
        {"AllowFriendlyFire", nbt::make_byte(team.allow_friendly_fire ? 1 : 0)},
        {"SeeFriendlyInvisibles", nbt::make_byte(team.see_friendly_invisibles ? 1 : 0)},
        {"NameTagVisibility", nbt::make_string(team_visibility_name(team.name_tag_visibility))},
        {"DeathMessageVisibility",
         nbt::make_string(team_visibility_name(team.death_message_visibility))},
        {"CollisionRule", nbt::make_string(team_collision_rule_name(team.collision_rule))},
        {"Prefix", nbt::make_string(team.prefix)},
        {"Suffix", nbt::make_string(team.suffix)},
        {"Players", nbt::make_list(Tag::String, std::move(players))},
    }));
  }

  return nbt::make_compound({
      {"DataVersion", nbt::make_int(kScoreboardStateDataVersion)},
      {"Objectives", nbt::make_list(Tag::Compound, std::move(objective_rows))},
      {"PlayerScores", nbt::make_list(Tag::Compound, std::move(score_rows))},
      {"DisplaySlots", nbt::make_compound(std::move(display_fields))},
      {"Teams", nbt::make_list(Tag::Compound, std::move(team_rows))},
  });
}

bool scoreboard_state_from_nbt(const nbt::Value& root, Scoreboard* out) {
  if (root.type != nbt::Tag::Compound)
    return false;
  Scoreboard board;

  // 1. Objectives, so every later section has something to attach to.
  const nbt::Value* objectives = nullptr;
  if (!list_field(root, "Objectives", nbt::Tag::Compound, &objectives))
    return false;
  if (objectives != nullptr) {
    for (const nbt::Value& row : objectives->items) {
      if (row.type != nbt::Tag::Compound) {
        log::warn("scoreboard.dat: skipping an objective row that is not a compound");
        continue;
      }
      const std::string name = string_field(row, "Name", {});
      const std::string criteria_name = string_field(row, "CriteriaName", {});
      if (name.empty() || criteria_name.empty()) {
        log::warn("scoreboard.dat: skipping an objective row with no name or criteria");
        continue;
      }
      const std::optional<ObjectiveCriteria> criteria = objective_criteria_by_name(criteria_name);
      if (!criteria) {
        // A criterion this build cannot resolve (a removed stat, a modded
        // name): its objective and every score on it are unreconstructible.
        log::warn("scoreboard.dat: skipping objective '", name, "' with unknown criteria '",
                  criteria_name, "'");
        continue;
      }
      if (!board.add_objective(name, *criteria, string_field(row, "DisplayName", name),
                               render_type_from_name(string_field(row, "RenderType", "integer")),
                               int_field(row, "DisplayAutoUpdate", 0) != 0)) {
        log::warn("scoreboard.dat: duplicate objective '", name, "'");
      }
    }
  }

  // 2. Scores, through the restore path (read-only stat criteria included).
  const nbt::Value* scores = nullptr;
  if (!list_field(root, "PlayerScores", nbt::Tag::Compound, &scores))
    return false;
  if (scores != nullptr) {
    for (const nbt::Value& row : scores->items) {
      if (row.type != nbt::Tag::Compound) {
        log::warn("scoreboard.dat: skipping a score row that is not a compound");
        continue;
      }
      const std::string owner = string_field(row, "Name", {});
      const std::string objective = string_field(row, "Objective", {});
      if (owner.empty() || objective.empty()) {
        log::warn("scoreboard.dat: skipping a score row with no holder or objective");
        continue;
      }
      if (board.objective(objective) == nullptr) {
        log::warn("scoreboard.dat: skipping score for '", owner, "' on unknown objective '",
                  objective, "'");
        continue;
      }
      board.restore_score(owner, objective, int_field(row, "Score", 0),
                          int_field(row, "Locked", 1) != 0);
    }
  }

  // 3. Display slots.
  if (const nbt::Value* slots = nbt::child(root, "DisplaySlots")) {
    if (slots->type != nbt::Tag::Compound) {
      log::warn("scoreboard.dat: DisplaySlots is not a compound");
    } else {
      for (const auto& [key, value] : slots->fields) {
        const int slot = display_slot_from_key(key);
        if (slot < 0) {
          log::warn("scoreboard.dat: skipping unknown display slot '", key, "'");
          continue;
        }
        const std::optional<std::string_view> shown = nbt::as_string(value);
        if (!shown || shown->empty()) {
          log::warn("scoreboard.dat: skipping display slot '", key, "' with no objective");
          continue;
        }
        if (board.objective(std::string(*shown)) == nullptr) {
          log::warn("scoreboard.dat: display slot '", key, "' names unknown objective '",
                    std::string(*shown), "'");
          continue;
        }
        board.set_display_objective(static_cast<uint8_t>(slot), std::string(*shown));
      }
    }
  }

  // 4. Teams and their members.
  const nbt::Value* teams = nullptr;
  if (!list_field(root, "Teams", nbt::Tag::Compound, &teams))
    return false;
  if (teams != nullptr) {
    for (const nbt::Value& row : teams->items) {
      if (row.type != nbt::Tag::Compound) {
        log::warn("scoreboard.dat: skipping a team row that is not a compound");
        continue;
      }
      const std::string name = string_field(row, "Name", {});
      if (name.empty()) {
        log::warn("scoreboard.dat: skipping a team row with no name");
        continue;
      }
      if (!board.add_team(name, nullptr)) {
        log::warn("scoreboard.dat: duplicate team '", name, "'");
        continue;
      }
      board.set_team_display_name(name, string_field(row, "DisplayName", name));
      const int color = int_field(row, "Color", -1);
      if (color >= 0 && color < kTeamColorCount)
        board.set_team_color(name, static_cast<uint8_t>(color));
      board.set_team_options(name, int_field(row, "AllowFriendlyFire", 1) != 0,
                             int_field(row, "SeeFriendlyInvisibles", 1) != 0);
      const int name_tag = team_visibility_id(string_field(row, "NameTagVisibility", "always"));
      const int death_message =
          team_visibility_id(string_field(row, "DeathMessageVisibility", "always"));
      board.set_team_visibility(
          name, static_cast<uint8_t>(name_tag < 0 ? kVisibilityAlways : name_tag),
          static_cast<uint8_t>(death_message < 0 ? kVisibilityAlways : death_message));
      const int collision = team_collision_rule_id(string_field(row, "CollisionRule", "always"));
      board.set_team_collision_rule(
          name, static_cast<uint8_t>(collision < 0 ? kCollisionAlways : collision));
      board.set_team_prefix_suffix(name, string_field(row, "Prefix", {}),
                                   string_field(row, "Suffix", {}));
      const nbt::Value* players = nullptr;
      if (!list_field(row, "Players", nbt::Tag::String, &players)) {
        log::warn("scoreboard.dat: team '", name, "' Players is not a list of names");
      } else if (players != nullptr) {
        for (const nbt::Value& player : players->items) {
          const std::optional<std::string_view> member = nbt::as_string(player);
          if (!member || member->empty()) {
            log::warn("scoreboard.dat: skipping a team member with no name (", name, ")");
            continue;
          }
          board.join_team(std::string(*member), name);
        }
      }
    }
  }

  // A load is not a set of changes a client hears about: the log it built while
  // restoring is dropped here, on the copy that becomes the server's board.
  board.take_changes();
  *out = std::move(board);
  return true;
}

bool write_scoreboard_state(const std::filesystem::path& world_dir, const Scoreboard& scoreboard) {
  std::error_code ec;
  if (!std::filesystem::is_directory(world_dir, ec)) {
    log::warn("scoreboard.dat: ", world_dir.string(), " is not a directory");
    return false;
  }
  const std::vector<uint8_t> nbt_bytes = nbt::serialize(scoreboard_state_to_nbt(scoreboard));
  std::vector<uint8_t> gz;
  if (!gzip_compress(nbt_bytes, &gz)) {
    log::error("scoreboard.dat: failed to compress the state");
    return false;
  }
  const std::filesystem::path path = world_dir / kScoreboardStateFileName;
  const std::filesystem::path tmp = world_dir / (std::string(kScoreboardStateFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      log::error("scoreboard.dat: cannot open ", tmp.string(), " for writing");
      return false;
    }
    out.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
    if (!out)
      return false;
  }
  std::error_code rename_ec;
  std::filesystem::rename(tmp, path, rename_ec);
  if (rename_ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
      log::error("scoreboard.dat: rename failed: ", rename_ec.message());
      return false;
    }
  }
  return true;
}

bool read_scoreboard_state(const std::filesystem::path& world_dir, Scoreboard* out) {
  const std::filesystem::path path = world_dir / kScoreboardStateFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return true;
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::warn("scoreboard.dat: cannot open ", path.string());
    return false;
  }
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
  std::vector<uint8_t> nbt_bytes;
  if (!inflate_any(raw.data(), raw.size(), &nbt_bytes)) {
    log::warn("scoreboard.dat: ", path.string(), " is not gzip NBT");
    return false;
  }
  const std::optional<nbt::Value> root = nbt::parse(nbt_bytes.data(), nbt_bytes.size());
  if (!root) {
    log::warn("scoreboard.dat: ", path.string(), " did not parse");
    return false;
  }
  return scoreboard_state_from_nbt(*root, out);
}

} // namespace mc::world
