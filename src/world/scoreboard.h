// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scoreboard (Phase 4 milestone M): objectives, their scores, the display
// slots and the teams — vanilla's `Scoreboard` / `Objective` / `Score` /
// `PlayerTeam`, plus the one piece the server needs and the model does not: a
// *change log* mirroring `ServerScoreboard`'s packet decisions, so the session
// can turn a mutation into the same packets vanilla broadcasts without the
// model ever knowing a socket exists.
//
// Two details worth spelling out, both from the decomp:
//
//   * An objective is *tracked* once a display slot shows it
//     (`ServerScoreboard.startTrackingObjective`); scores on an untracked
//     objective never reach a client, and the tracking start sends the
//     objective, its display slots and every score it already carries.
//   * Criteria are indexed by name. Vanilla indexes by the `ObjectiveCriteria`
//     *object* (`getOrCreatePlayerScore(forceWritable)` and
//     `forAllObjectives`), and its stat criteria are singletons per stat, so a
//     name is equivalent for every criterion this engine resolves — including
//     the 77 * 3 + 2 stat names `ObjectiveCriteria.byName` can build.
//
// Display names, prefixes and suffixes are plain strings here where vanilla
// carries a `Component`: the session renders each one as the literal text
// component every other title in this server already uses, and a
// translatable/styled component is documented as a limitation.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "world/stats.h"

namespace mc::world {

// ---- criteria --------------------------------------------------------------

enum ScoreboardRenderType : uint8_t { kRenderInteger = 0, kRenderHearts = 1 };

// `DisplaySlot`: the packet writes the id.
enum DisplaySlotId : uint8_t {
  kDisplayList = 0,
  kDisplaySidebar = 1,
  kDisplayBelowName = 2,
  // 3..18 are the per-team-colour sidebars (`sidebar.team.<colour>`), one per
  // TeamColor id 0..15 — `DISPLAY_SLOT_TEAM_COLOR_FIRST + colour`.
  kDisplayTeamColorFirst = 3,
  kDisplaySlotCount = 19,
};

// `TeamColor`: the packet and the save file carry the id; `/scoreboard teams
// modify <team> color <name>` takes the name.
enum TeamColorId : uint8_t { kTeamColorCount = 16 };
const char* team_color_name(uint8_t id);  // "" out of range
int team_color_id(std::string_view name); // -1 unknown

// `Team.Visibility` / `Team.CollisionRule` ids, in enum order.
enum TeamVisibility : uint8_t {
  kVisibilityAlways = 0,
  kVisibilityNever = 1,
  kVisibilityHideForOtherTeams = 2,
  kVisibilityHideForOwnTeam = 3,
  kTeamVisibilityCount = 4,
};
enum TeamCollisionRule : uint8_t {
  kCollisionAlways = 0,
  kCollisionNever = 1,
  kCollisionPushOtherTeams = 2,
  kCollisionPushOwnTeam = 3,
  kTeamCollisionRuleCount = 4,
};
const char* team_visibility_name(uint8_t id); // packet/save spelling
int team_visibility_id(std::string_view name);
const char* team_collision_rule_name(uint8_t id);
int team_collision_rule_id(std::string_view name);

// The six read-only custom criteria whose value the player tick mirrors
// (`ServerPlayer.updateScoreForCriteria`).
enum CustomCriterion : int8_t {
  kCriterionNone = -1,
  kCriterionHealth = 0,
  kCriterionFood = 1,
  kCriterionAir = 2,
  kCriterionArmor = 3,
  kCriterionExperience = 4,
  kCriterionLevel = 5,
};

// `ObjectiveCriteria.byName`: the custom criteria table first (dummy, trigger,
// deathCount, playerKillCount, totalKillCount, health, food, air, armor, xp,
// level, teamkill.<colour>, killedByTeam.<colour>), then a stat name.
struct ObjectiveCriteria {
  std::string name;
  bool read_only = false;
  uint8_t default_render_type = kRenderInteger;
  // Set when the criterion is one of the six the player tick sets by value.
  int8_t custom_criterion = kCriterionNone;
  // The two custom criteria this engine keeps in sync from the award sites
  // (`ServerPlayer`'s `forAllObjectives(DEATH_COUNT/KILL_COUNT_*, ...)` runs).
  enum Auto : uint8_t {
    kAutoNone = 0,
    kAutoDeathCount,
    kAutoPlayerKillCount,
    kAutoTotalKillCount,
  } auto_kind = kAutoNone;
  bool is_stat = false;
  StatKey stat{};
};
// std::nullopt when vanilla's `byName` would say "No scoreboard criteria with
// name". `custom_criteria_names()` is `getCustomCriteriaNames()`.
std::optional<ObjectiveCriteria> objective_criteria_by_name(std::string_view name);
std::vector<std::string_view> custom_criteria_names();

// ---- the model -------------------------------------------------------------

struct Objective {
  std::string name;
  ObjectiveCriteria criteria;
  std::string display_name;
  uint8_t render_type = kRenderInteger;
  bool display_auto_update = false;
  bool tracked = false;
};

struct Score {
  int value = 0;
  bool locked = true; // Score's own field initializer
};

struct PlayerScoreEntry {
  std::string owner;
  int value = 0;
};

struct Team {
  std::string name;
  std::string display_name;
  bool has_color = false;
  uint8_t color = 0;
  bool allow_friendly_fire = true;
  bool see_friendly_invisibles = true;
  std::string prefix;
  std::string suffix;
  uint8_t name_tag_visibility = kVisibilityAlways;
  uint8_t death_message_visibility = kVisibilityAlways;
  uint8_t collision_rule = kCollisionAlways;
  std::set<std::string> players;
};

// One entry of the change log; the session's `apply_scoreboard_changes` turns
// each into the packet `ServerScoreboard` would have sent.
enum class ScoreboardChangeKind : uint8_t {
  kObjectiveAdded,
  kObjectiveChanged,
  kObjectiveRemoved,
  kScoreChanged,
  kScoreRemoved,
  kPlayerScoresCleared, // onPlayerRemoved: reset_score with a null objective
  kDisplayObjectiveSet,
  kTrackingStarted,
  kTrackingStopped,
  kTeamAdded,
  kTeamChanged,
  kTeamRemoved,
  kTeamPlayerJoined,
  kTeamPlayerLeft,
};

struct ScoreboardChange {
  // Defaults on every field so the call sites can spell only the ones a given
  // event carries (the omitted ones are the empty string / zero).
  ScoreboardChangeKind kind = ScoreboardChangeKind::kObjectiveAdded;
  std::string objective = {};
  std::string owner = {};
  std::string team = {};
  std::string player = {};
  uint8_t slot = 0;
  int value = 0;
};

class Scoreboard {
public:
  // ---- objectives ----
  // `addObjective`: false when the name is taken (vanilla throws).
  bool add_objective(std::string name, ObjectiveCriteria criteria, std::string display_name,
                     uint8_t render_type, bool display_auto_update);
  // `removeObjective`: drops its scores, clears every display slot showing it,
  // and stops its tracking.
  bool remove_objective(const std::string& name);
  Objective* objective(const std::string& name);
  const Objective* objective(const std::string& name) const;
  const std::map<std::string, Objective>& objectives() const {
    return objectives_;
  }
  // `Objective.setDisplayName` / `setRenderType` / `setDisplayAutoUpdate`.
  bool set_objective_display_name(const std::string& name, std::string display_name);
  bool set_objective_render_type(const std::string& name, uint8_t render_type);

  // ---- scores ----
  // `getOrCreatePlayerScore(...).set`: false on a read-only criterion, which is
  // vanilla's "Cannot modify read-only score".
  bool set_score(const std::string& owner, const std::string& objective, int value);
  // `ScoreAccess.add` / `increment`.
  bool add_score(const std::string& owner, const std::string& objective, int count);
  // `ScoreAccess.lock` / `unlock` (the `enable` command).
  bool set_score_locked(const std::string& owner, const std::string& objective, bool locked);
  // `ScoreboardSaveData.load`'s write path: the saved value and lock land in
  // the model without the read-only check, because a stat criterion's scores
  // are exactly what the file carries, and without a change-log entry, because
  // a load is not a mutation any client hears about. False only for an unknown
  // objective.
  bool restore_score(const std::string& owner, const std::string& objective, int value,
                     bool locked);
  int score(const std::string& owner, const std::string& objective) const;
  bool has_score(const std::string& owner, const std::string& objective) const;
  bool score_locked(const std::string& owner, const std::string& objective) const;
  // `resetSinglePlayerScore` / `resetAllPlayerScores`.
  bool reset_score(const std::string& owner, const std::string& objective);
  bool reset_all_scores(const std::string& owner);
  const std::map<std::string, Score>& scores(const std::string& objective) const;
  // `listPlayerScores(objective)`, name order (the map's own).
  std::vector<PlayerScoreEntry> list_scores(const std::string& objective) const;

  // ---- display slots ----
  bool set_display_objective(uint8_t slot, const std::string& objective);
  bool clear_display_objective(uint8_t slot);
  const std::string* display_objective_name(uint8_t slot) const; // nullptr = none
  int display_slot_count(const std::string& objective) const;

  // ---- teams ----
  bool add_team(std::string name, Team* out);
  bool remove_team(const std::string& name);
  Team* team(const std::string& name);
  const std::map<std::string, Team>& teams() const {
    return teams_;
  }
  // `addPlayerToTeam`: moves a member from whatever team it is on
  // (`Routes T1: getPlayersTeam` first).
  bool join_team(const std::string& player_name, const std::string& team_name);
  bool leave_team(const std::string& player_name);
  Team* players_team(const std::string& player_name);
  // `PlayerTeam.setDisplayName` and friends; false for an unknown team.
  bool set_team_display_name(const std::string& name, std::string display_name);
  bool set_team_color(const std::string& name, std::optional<uint8_t> color);
  bool set_team_options(const std::string& name, bool allow_friendly_fire, bool see_invisibles);
  bool set_team_prefix_suffix(const std::string& name, std::string prefix, std::string suffix);
  bool set_team_visibility(const std::string& name, uint8_t name_tag, uint8_t death_message);
  bool set_team_collision_rule(const std::string& name, uint8_t rule);

  // ---- criteria hookups ----
  // `ServerPlayer.awardStat`'s `forAllObjectives(stat, holder, score -> add)`.
  void add_stat_score(const StatKey& stat, const std::string& owner, int count);
  // `forAllObjectives(ObjectiveCriteria.X, holder, increment)`.
  void increment_auto_criteria(ObjectiveCriteria::Auto kind, const std::string& owner);
  // `ServerPlayer.updateScoreForCriteria` (`score -> set`), the read-only
  // criteria like health and level.
  void set_custom_criteria_score(CustomCriterion criterion, const std::string& owner, int value);

  // ---- the change log ----
  std::vector<ScoreboardChange> take_changes();
  bool dirty() const {
    return !changes_.empty();
  }

private:
  void push(ScoreboardChange change) {
    changes_.push_back(std::move(change));
  }
  // `ServerScoreboard.setDisplayObjective`'s tracking half.
  void start_tracking(Objective* objective);
  void stop_tracking(Objective* objective);

  std::map<std::string, Objective> objectives_;
  std::map<std::string, std::map<std::string, Score>> scores_; // objective -> owner -> score
  std::map<std::string, std::string> players_team_;            // player -> team name
  std::map<std::string, Team> teams_;
  std::optional<std::string> display_[kDisplaySlotCount];
  std::vector<ScoreboardChange> changes_;
};

} // namespace mc::world
