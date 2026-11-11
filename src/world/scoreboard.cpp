// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scoreboard model: see world/scoreboard.h for the shape and the decomp
// this mirrors (`Scoreboard`, `Objective`, `Score`, `PlayerTeam` and the
// `ServerScoreboard` overrides that decide which changes become packets).
#include "world/scoreboard.h"

#include <algorithm>
#include <array>

namespace mc::world {
namespace {

constexpr const char* kTeamColorNames[kTeamColorCount] = {
    "black", "dark_blue",    "dark_green", "dark_aqua", "dark_red", "dark_purple",
    "gold",  "gray",         "dark_gray",  "blue",      "green",    "aqua",
    "red",   "light_purple", "yellow",     "white",
};

constexpr const char* kVisibilityNames[kTeamVisibilityCount] = {
    "always",
    "never",
    "hideForOtherTeams",
    "hideForOwnTeam",
};

constexpr const char* kCollisionNames[kTeamCollisionRuleCount] = {
    "always",
    "never",
    "pushOtherTeams",
    "pushOwnTeam",
};

} // namespace

const char* team_color_name(uint8_t id) {
  return id < kTeamColorCount ? kTeamColorNames[id] : "";
}

int team_color_id(std::string_view name) {
  for (uint8_t i = 0; i < kTeamColorCount; ++i)
    if (name == kTeamColorNames[i])
      return static_cast<int>(i);
  return -1;
}

const char* team_visibility_name(uint8_t id) {
  return id < kTeamVisibilityCount ? kVisibilityNames[id] : "";
}

int team_visibility_id(std::string_view name) {
  for (uint8_t i = 0; i < kTeamVisibilityCount; ++i)
    if (name == kVisibilityNames[i])
      return static_cast<int>(i);
  return -1;
}

const char* team_collision_rule_name(uint8_t id) {
  return id < kTeamCollisionRuleCount ? kCollisionNames[id] : "";
}

int team_collision_rule_id(std::string_view name) {
  for (uint8_t i = 0; i < kTeamCollisionRuleCount; ++i)
    if (name == kCollisionNames[i])
      return static_cast<int>(i);
  return -1;
}

std::optional<ObjectiveCriteria> objective_criteria_by_name(std::string_view name) {
  // The custom table, in `ObjectiveCriteria`'s field order.
  struct Row {
    const char* name;
    bool read_only;
    uint8_t render_type;
  };
  static constexpr Row kRows[] = {
      {"dummy", false, kRenderInteger},
      {"trigger", false, kRenderInteger},
      {"deathCount", false, kRenderInteger},
      {"playerKillCount", false, kRenderInteger},
      {"totalKillCount", false, kRenderInteger},
      {"health", true, kRenderHearts},
      {"food", true, kRenderInteger},
      {"air", true, kRenderInteger},
      {"armor", true, kRenderInteger},
      {"xp", true, kRenderInteger},
      {"level", true, kRenderInteger},
  };
  for (const Row& row : kRows) {
    if (name != row.name)
      continue;
    ObjectiveCriteria criteria;
    criteria.name = row.name;
    criteria.read_only = row.read_only;
    criteria.default_render_type = row.render_type;
    if (name == "deathCount")
      criteria.auto_kind = ObjectiveCriteria::kAutoDeathCount;
    else if (name == "playerKillCount")
      criteria.auto_kind = ObjectiveCriteria::kAutoPlayerKillCount;
    else if (name == "totalKillCount")
      criteria.auto_kind = ObjectiveCriteria::kAutoTotalKillCount;
    else if (name == "health")
      criteria.custom_criterion = kCriterionHealth;
    else if (name == "food")
      criteria.custom_criterion = kCriterionFood;
    else if (name == "air")
      criteria.custom_criterion = kCriterionAir;
    else if (name == "armor")
      criteria.custom_criterion = kCriterionArmor;
    else if (name == "xp")
      criteria.custom_criterion = kCriterionExperience;
    else if (name == "level")
      criteria.custom_criterion = kCriterionLevel;
    return criteria;
  }
  // `registerForEveryTeamColor`: teamkill.<colour> and killedByTeam.<colour>,
  // sixteen each, all writable — they are the criteria `handleTeamKill` moves.
  for (const char* prefix : {"teamkill.", "killedByTeam."}) {
    const std::string_view prefix_view(prefix);
    if (name.size() <= prefix_view.size() || name.substr(0, prefix_view.size()) != prefix_view)
      continue;
    const int color = team_color_id(name.substr(prefix_view.size()));
    if (color < 0)
      continue;
    ObjectiveCriteria criteria;
    criteria.name = std::string(name);
    criteria.default_render_type = kRenderInteger;
    return criteria;
  }
  // Then the stat branch: every stat is an ObjectiveCriteria and is read-only.
  if (std::optional<StatKey> stat = stat_from_criterion_name(name)) {
    ObjectiveCriteria criteria;
    criteria.name = std::string(name);
    criteria.read_only = true;
    criteria.is_stat = true;
    criteria.stat = *stat;
    return criteria;
  }
  return std::nullopt;
}

std::vector<std::string_view> custom_criteria_names() {
  return {"dummy",          "trigger", "deathCount", "playerKillCount",
          "totalKillCount", "health",  "food",       "air",
          "armor",          "xp",      "level"};
}

// ---- objectives ------------------------------------------------------------

bool Scoreboard::add_objective(std::string name, ObjectiveCriteria criteria,
                               std::string display_name, uint8_t render_type,
                               bool display_auto_update) {
  if (objectives_.count(name) != 0)
    return false;
  Objective objective;
  objective.name = name;
  objective.criteria = std::move(criteria);
  objective.display_name = std::move(display_name);
  objective.render_type = render_type;
  objective.display_auto_update = display_auto_update;
  const std::string added = name;
  objectives_.emplace(std::move(name), std::move(objective));
  push(ScoreboardChange{ScoreboardChangeKind::kObjectiveAdded, added});
  return true;
}

bool Scoreboard::remove_objective(const std::string& name) {
  const auto it = objectives_.find(name);
  if (it == objectives_.end())
    return false;
  // `removeObjective`: every display slot showing it goes first (each one is a
  // `setDisplayObjective(slot, null)`), then its scores, then the objective.
  for (uint8_t slot = 0; slot < kDisplaySlotCount; ++slot)
    if (display_[slot].has_value() && *display_[slot] == name)
      clear_display_objective(slot);
  scores_.erase(name);
  if (it->second.tracked) {
    it->second.tracked = false;
    push(ScoreboardChange{ScoreboardChangeKind::kTrackingStopped, name});
  }
  objectives_.erase(it);
  push(ScoreboardChange{ScoreboardChangeKind::kObjectiveRemoved, name});
  return true;
}

Objective* Scoreboard::objective(const std::string& name) {
  const auto it = objectives_.find(name);
  return it == objectives_.end() ? nullptr : &it->second;
}

const Objective* Scoreboard::objective(const std::string& name) const {
  const auto it = objectives_.find(name);
  return it == objectives_.end() ? nullptr : &it->second;
}

bool Scoreboard::set_objective_display_name(const std::string& name, std::string display_name) {
  Objective* objective_ptr = objective(name);
  if (objective_ptr == nullptr || objective_ptr->display_name == display_name)
    return false;
  objective_ptr->display_name = std::move(display_name);
  if (objective_ptr->tracked)
    push(ScoreboardChange{ScoreboardChangeKind::kObjectiveChanged, name});
  return true;
}

bool Scoreboard::set_objective_render_type(const std::string& name, uint8_t render_type) {
  Objective* objective_ptr = objective(name);
  if (objective_ptr == nullptr || objective_ptr->render_type == render_type)
    return false;
  objective_ptr->render_type = render_type;
  if (objective_ptr->tracked)
    push(ScoreboardChange{ScoreboardChangeKind::kObjectiveChanged, name});
  return true;
}

// ---- scores ----------------------------------------------------------------

namespace {
// The force flag is vanilla's `getOrCreatePlayerScore(forceWritable)`: the
// award sites write read-only criteria (a stat criterion is read-only to a
// command but is exactly what `forAllObjectives` sets), so the engine needs a
// path that skips the check.
bool set_score_internal(Scoreboard& scoreboard_ref,
                        std::map<std::string, std::map<std::string, Score>>* scores,
                        const std::string& owner, const std::string& objective_name, int value,
                        bool force, bool* changed) {
  Objective* objective_ptr = scoreboard_ref.objective(objective_name);
  if (objective_ptr == nullptr)
    return false;
  if (!force && objective_ptr->criteria.read_only)
    return false;
  std::map<std::string, Score>& by_owner = (*scores)[objective_name];
  const auto it = by_owner.find(owner);
  const bool created = it == by_owner.end();
  if (created) {
    by_owner[owner] = Score{value, true};
    *changed = true;
    return true;
  }
  if (it->second.value != value) {
    it->second.value = value;
    *changed = true;
  }
  return true;
}
} // namespace

bool Scoreboard::set_score(const std::string& owner, const std::string& objective_name, int value) {
  bool changed = false;
  if (!set_score_internal(*this, &scores_, owner, objective_name, value, false, &changed))
    return false;
  if (changed && objective(objective_name)->tracked)
    push(ScoreboardChange{
        ScoreboardChangeKind::kScoreChanged, objective_name, owner, {}, {}, 0, value});
  return true;
}

bool Scoreboard::add_score(const std::string& owner, const std::string& objective_name, int count) {
  const int next = score(owner, objective_name) + count;
  return set_score(owner, objective_name, next);
}

bool Scoreboard::set_score_locked(const std::string& owner, const std::string& objective_name,
                                  bool locked) {
  const auto objective_it = scores_.find(objective_name);
  if (objective_it == scores_.end())
    return false;
  const auto score_it = objective_it->second.find(owner);
  if (score_it == objective_it->second.end())
    return false;
  if (score_it->second.locked == locked)
    return false;
  score_it->second.locked = locked;
  // `onScoreLockChanged` marks the save dirty; nothing is sent on the wire.
  return true;
}

bool Scoreboard::restore_score(const std::string& owner, const std::string& objective_name,
                               int value, bool locked) {
  if (objective(objective_name) == nullptr)
    return false;
  bool changed = false;
  if (!set_score_internal(*this, &scores_, owner, objective_name, value, true, &changed))
    return false;
  scores_[objective_name][owner].locked = locked;
  return true;
}

int Scoreboard::score(const std::string& owner, const std::string& objective_name) const {
  const auto objective_it = scores_.find(objective_name);
  if (objective_it == scores_.end())
    return 0;
  const auto score_it = objective_it->second.find(owner);
  return score_it == objective_it->second.end() ? 0 : score_it->second.value;
}

bool Scoreboard::has_score(const std::string& owner, const std::string& objective_name) const {
  const auto objective_it = scores_.find(objective_name);
  if (objective_it == scores_.end())
    return false;
  return objective_it->second.count(owner) != 0;
}

bool Scoreboard::score_locked(const std::string& owner, const std::string& objective_name) const {
  const auto objective_it = scores_.find(objective_name);
  if (objective_it == scores_.end())
    return false;
  const auto score_it = objective_it->second.find(owner);
  return score_it != objective_it->second.end() && score_it->second.locked;
}

const std::map<std::string, Score>& Scoreboard::scores(const std::string& objective_name) const {
  static const std::map<std::string, Score> kEmpty;
  const auto it = scores_.find(objective_name);
  return it == scores_.end() ? kEmpty : it->second;
}

bool Scoreboard::reset_score(const std::string& owner, const std::string& objective_name) {
  const auto objective_it = scores_.find(objective_name);
  if (objective_it == scores_.end())
    return false;
  const auto score_it = objective_it->second.find(owner);
  if (score_it == objective_it->second.end())
    return false;
  const bool was_tracked = [&] {
    const Objective* objective_ptr = objective(objective_name);
    return objective_ptr != nullptr && objective_ptr->tracked;
  }();
  objective_it->second.erase(score_it);
  if (was_tracked)
    push(ScoreboardChange{ScoreboardChangeKind::kScoreRemoved, objective_name, owner});
  if (objective_it->second.empty()) {
    scores_.erase(objective_it);
    // `resetSinglePlayerScore` drops the holder entirely, which is the
    // `onPlayerRemoved` broadcast (reset_score with no objective).
    push(ScoreboardChange{ScoreboardChangeKind::kPlayerScoresCleared, {}, owner});
  }
  return true;
}

bool Scoreboard::reset_all_scores(const std::string& owner) {
  bool removed = false;
  for (auto& entry : scores_) {
    if (entry.second.erase(owner) != 0)
      removed = true;
  }
  for (auto it = scores_.begin(); it != scores_.end();) {
    if (it->second.empty())
      it = scores_.erase(it);
    else
      ++it;
  }
  if (removed)
    push(ScoreboardChange{ScoreboardChangeKind::kPlayerScoresCleared, {}, owner});
  return removed;
}

std::vector<PlayerScoreEntry> Scoreboard::list_scores(const std::string& objective_name) const {
  std::vector<PlayerScoreEntry> out;
  for (const auto& entry : scores(objective_name))
    out.push_back(PlayerScoreEntry{entry.first, entry.second.value});
  return out;
}

// ---- display slots ---------------------------------------------------------

bool Scoreboard::set_display_objective(uint8_t slot, const std::string& objective_name) {
  if (slot >= kDisplaySlotCount)
    return false;
  Objective* next = objective(objective_name);
  if (next == nullptr)
    return false;
  const std::string* old_name = display_objective_name(slot);
  Objective* old = old_name != nullptr ? objective(*old_name) : nullptr;
  if (old == next)
    return true;
  display_[slot] = objective_name;
  // `ServerScoreboard.setDisplayObjective`: the replaced objective leaves the
  // slot's screen (a display packet when it is still shown elsewhere, a full
  // stop-tracking when this was its last slot), and the new one either gets a
  // display packet (already tracked) or starts tracking, which is what sends
  // its objective, slots and scores.
  if (old != nullptr) {
    if (display_slot_count(old->name) > 0)
      push(ScoreboardChange{
          ScoreboardChangeKind::kDisplayObjectiveSet, next->name, {}, {}, {}, slot});
    else
      stop_tracking(old);
  }
  if (next->tracked)
    push(
        ScoreboardChange{ScoreboardChangeKind::kDisplayObjectiveSet, next->name, {}, {}, {}, slot});
  else
    start_tracking(next);
  return true;
}

bool Scoreboard::clear_display_objective(uint8_t slot) {
  if (slot >= kDisplaySlotCount)
    return false;
  const std::string* old_name = display_objective_name(slot);
  if (old_name == nullptr)
    return false;
  Objective* old = objective(*old_name);
  display_[slot].reset();
  if (old != nullptr) {
    if (display_slot_count(old->name) > 0)
      push(ScoreboardChange{ScoreboardChangeKind::kDisplayObjectiveSet, {}, {}, {}, {}, slot});
    else
      stop_tracking(old);
  }
  return true;
}

const std::string* Scoreboard::display_objective_name(uint8_t slot) const {
  if (slot >= kDisplaySlotCount || !display_[slot].has_value())
    return nullptr;
  return &*display_[slot];
}

int Scoreboard::display_slot_count(const std::string& objective_name) const {
  int count = 0;
  for (const std::optional<std::string>& shown : display_)
    if (shown.has_value() && *shown == objective_name)
      ++count;
  return count;
}

void Scoreboard::start_tracking(Objective* objective_ptr) {
  objective_ptr->tracked = true;
  push(ScoreboardChange{ScoreboardChangeKind::kTrackingStarted, objective_ptr->name});
}

void Scoreboard::stop_tracking(Objective* objective_ptr) {
  if (!objective_ptr->tracked)
    return;
  objective_ptr->tracked = false;
  push(ScoreboardChange{ScoreboardChangeKind::kTrackingStopped, objective_ptr->name});
}

// ---- teams -----------------------------------------------------------------

bool Scoreboard::add_team(std::string name, Team* out) {
  if (teams_.count(name) != 0)
    return false;
  Team team;
  team.name = name;
  team.display_name = name; // PlayerTeam's constructor uses the team's name
  const std::string added = name;
  Team& stored = teams_.emplace(std::move(name), std::move(team)).first->second;
  if (out != nullptr)
    *out = stored;
  push(ScoreboardChange{ScoreboardChangeKind::kTeamAdded, {}, {}, added});
  return true;
}

bool Scoreboard::remove_team(const std::string& name) {
  const auto it = teams_.find(name);
  if (it == teams_.end())
    return false;
  for (const std::string& player_name : it->second.players)
    players_team_.erase(player_name);
  teams_.erase(it);
  push(ScoreboardChange{ScoreboardChangeKind::kTeamRemoved, {}, {}, name});
  return true;
}

Team* Scoreboard::team(const std::string& name) {
  const auto it = teams_.find(name);
  return it == teams_.end() ? nullptr : &it->second;
}

Team* Scoreboard::players_team(const std::string& player_name) {
  const auto it = players_team_.find(player_name);
  return it == players_team_.end() ? nullptr : team(it->second);
}

bool Scoreboard::join_team(const std::string& player_name, const std::string& team_name) {
  Team* target = team(team_name);
  if (target == nullptr)
    return false;
  const auto current = players_team_.find(player_name);
  if (current != players_team_.end() && current->second == team_name)
    return true;
  if (current != players_team_.end()) {
    const std::string old_team = current->second;
    if (Team* old = team(old_team); old != nullptr)
      old->players.erase(player_name);
    push(ScoreboardChange{ScoreboardChangeKind::kTeamPlayerLeft, {}, {}, old_team, player_name});
  }
  target->players.insert(player_name);
  players_team_[player_name] = team_name;
  push(ScoreboardChange{ScoreboardChangeKind::kTeamPlayerJoined, {}, {}, team_name, player_name});
  return true;
}

bool Scoreboard::leave_team(const std::string& player_name) {
  const auto it = players_team_.find(player_name);
  if (it == players_team_.end())
    return false;
  const std::string team_name = it->second;
  if (Team* current = team(team_name); current != nullptr)
    current->players.erase(player_name);
  players_team_.erase(it);
  push(ScoreboardChange{ScoreboardChangeKind::kTeamPlayerLeft, {}, {}, team_name, player_name});
  return true;
}

bool Scoreboard::set_team_display_name(const std::string& name, std::string display_name) {
  Team* target = team(name);
  if (target == nullptr || target->display_name == display_name)
    return false;
  target->display_name = std::move(display_name);
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

bool Scoreboard::set_team_color(const std::string& name, std::optional<uint8_t> color) {
  Team* target = team(name);
  if (target == nullptr)
    return false;
  if (target->has_color == color.has_value() && (!color.has_value() || target->color == *color))
    return false;
  target->has_color = color.has_value();
  target->color = color.value_or(0);
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

bool Scoreboard::set_team_options(const std::string& name, bool allow_friendly_fire,
                                  bool see_invisibles) {
  Team* target = team(name);
  if (target == nullptr)
    return false;
  if (target->allow_friendly_fire == allow_friendly_fire &&
      target->see_friendly_invisibles == see_invisibles)
    return false;
  target->allow_friendly_fire = allow_friendly_fire;
  target->see_friendly_invisibles = see_invisibles;
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

bool Scoreboard::set_team_prefix_suffix(const std::string& name, std::string prefix,
                                        std::string suffix) {
  Team* target = team(name);
  if (target == nullptr)
    return false;
  if (target->prefix == prefix && target->suffix == suffix)
    return false;
  target->prefix = std::move(prefix);
  target->suffix = std::move(suffix);
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

bool Scoreboard::set_team_visibility(const std::string& name, uint8_t name_tag,
                                     uint8_t death_message) {
  Team* target = team(name);
  if (target == nullptr)
    return false;
  if (target->name_tag_visibility == name_tag && target->death_message_visibility == death_message)
    return false;
  target->name_tag_visibility = name_tag;
  target->death_message_visibility = death_message;
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

bool Scoreboard::set_team_collision_rule(const std::string& name, uint8_t rule) {
  Team* target = team(name);
  if (target == nullptr || target->collision_rule == rule)
    return false;
  target->collision_rule = rule;
  push(ScoreboardChange{ScoreboardChangeKind::kTeamChanged, {}, {}, name});
  return true;
}

// ---- criteria hookups ------------------------------------------------------

void Scoreboard::add_stat_score(const StatKey& stat, const std::string& owner, int count) {
  for (auto& entry : objectives_) {
    Objective& objective_entry = entry.second;
    if (!objective_entry.criteria.is_stat || !(objective_entry.criteria.stat == stat))
      continue;
    bool changed = false;
    const int next = score(owner, entry.first) + count;
    if (set_score_internal(*this, &scores_, owner, entry.first, next, true, &changed) && changed &&
        objective_entry.tracked)
      push(ScoreboardChange{
          ScoreboardChangeKind::kScoreChanged, entry.first, owner, {}, {}, 0, next});
  }
}

void Scoreboard::increment_auto_criteria(ObjectiveCriteria::Auto kind, const std::string& owner) {
  if (kind == ObjectiveCriteria::kAutoNone)
    return;
  for (auto& entry : objectives_) {
    Objective& objective_entry = entry.second;
    if (objective_entry.criteria.auto_kind != kind)
      continue;
    bool changed = false;
    const int next = score(owner, entry.first) + 1;
    if (set_score_internal(*this, &scores_, owner, entry.first, next, true, &changed) && changed &&
        objective_entry.tracked)
      push(ScoreboardChange{
          ScoreboardChangeKind::kScoreChanged, entry.first, owner, {}, {}, 0, next});
  }
}

void Scoreboard::set_custom_criteria_score(CustomCriterion criterion, const std::string& owner,
                                           int value) {
  if (criterion == kCriterionNone)
    return;
  for (auto& entry : objectives_) {
    Objective& objective_entry = entry.second;
    if (objective_entry.criteria.custom_criterion != criterion)
      continue;
    bool changed = false;
    if (set_score_internal(*this, &scores_, owner, entry.first, value, true, &changed) && changed &&
        objective_entry.tracked)
      push(ScoreboardChange{
          ScoreboardChangeKind::kScoreChanged, entry.first, owner, {}, {}, 0, value});
  }
}

std::vector<ScoreboardChange> Scoreboard::take_changes() {
  std::vector<ScoreboardChange> out = std::move(changes_);
  changes_.clear();
  return out;
}

} // namespace mc::world
