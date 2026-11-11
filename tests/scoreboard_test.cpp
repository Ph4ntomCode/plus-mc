// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the scoreboard model (Phase 4 milestone M):
//   * `ObjectiveCriteria.byName`: the eleven custom names with their read-only
//     flags and default render types (health is HEARTS), the two team-colour
//     families, the stat branch (read-only), and the null for a name vanilla
//     rejects;
//   * objectives: add/duplicate/remove, the display-slot tracking
//     (`startTrackingObjective` sending the objective + its slots + scores, the
//     stop when the last slot goes), and a display change that only moves a
//     slot when the objective is shown elsewhere;
//   * scores: set/add/create, the read-only refusal on the command path versus
//     the forced stat path, list order, per-objective and per-holder resets
//     with their `ResetScore` events;
//   * the stat and count criteria hookups: only the matching objectives move,
//     and only a *tracked* one emits a score change;
//   * teams: add/remove/join/leave/move, the option setters' change events, and
//     the two name tables the packet and the save file write.
#include <cstdio>
#include <string>
#include <vector>

#include "world/scoreboard.h"
#include "world/stats.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    const std::string va = (a), vb = (b);                                                          \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a,      \
                   va.c_str(), vb.c_str());                                                        \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

bool has_change(const std::vector<ScoreboardChange>& changes, ScoreboardChangeKind kind,
                const std::string& objective = {}, const std::string& owner = {}) {
  for (const ScoreboardChange& change : changes) {
    if (change.kind != kind)
      continue;
    if (!objective.empty() && change.objective != objective)
      continue;
    if (!owner.empty() && change.owner != owner)
      continue;
    return true;
  }
  return false;
}

int change_count(const std::vector<ScoreboardChange>& changes, ScoreboardChangeKind kind) {
  int count = 0;
  for (const ScoreboardChange& change : changes)
    if (change.kind == kind)
      ++count;
  return count;
}

} // namespace

int main() {
  // ---- criteria ----
  const std::optional<ObjectiveCriteria> dummy = objective_criteria_by_name("dummy");
  CHECK(dummy.has_value());
  CHECK(dummy.has_value() && !dummy->read_only && dummy->default_render_type == kRenderInteger);
  const std::optional<ObjectiveCriteria> health = objective_criteria_by_name("health");
  CHECK(health.has_value());
  CHECK(health.has_value() && health->read_only && health->default_render_type == kRenderHearts);
  CHECK(health.has_value() && health->custom_criterion == kCriterionHealth);
  CHECK_EQ(objective_criteria_by_name("deathCount")->auto_kind, ObjectiveCriteria::kAutoDeathCount);
  CHECK_EQ(objective_criteria_by_name("playerKillCount")->auto_kind,
           ObjectiveCriteria::kAutoPlayerKillCount);
  CHECK_EQ(objective_criteria_by_name("totalKillCount")->auto_kind,
           ObjectiveCriteria::kAutoTotalKillCount);
  CHECK(!objective_criteria_by_name("deathCount")->read_only);
  CHECK_EQ(objective_criteria_by_name("level")->custom_criterion, kCriterionLevel);
  CHECK(objective_criteria_by_name("teamkill.red").has_value());
  CHECK(objective_criteria_by_name("killedByTeam.dark_aqua").has_value());
  CHECK(!objective_criteria_by_name("teamkill.chartreuse").has_value());
  const std::optional<ObjectiveCriteria> stat_criterion =
      objective_criteria_by_name("minecraft.custom:minecraft.mob_kills");
  CHECK(stat_criterion.has_value());
  CHECK(stat_criterion.has_value() && stat_criterion->read_only && stat_criterion->is_stat);
  CHECK(stat_criterion.has_value() && stat_criterion->stat.type == kStatCustom &&
        stat_criterion->stat.value == kCustomMobKills);
  CHECK(!objective_criteria_by_name("minecraft.custom:minecraft.not_a_stat").has_value());
  CHECK(!objective_criteria_by_name("not_a_criterion").has_value());
  CHECK_EQ(custom_criteria_names().size(), 11u);
  CHECK_STR(std::string(custom_criteria_names()[0]), "dummy");

  // ---- objectives ----
  Scoreboard board;
  CHECK(board.add_objective("points", *dummy, "Points", kRenderInteger, false));
  CHECK(!board.add_objective("points", *dummy, "Points", kRenderInteger, false));
  CHECK(board.objective("points") != nullptr);
  CHECK(board.take_changes().size() == 1u); // the add
  CHECK(board.add_objective("kills",
                            *objective_criteria_by_name("minecraft.custom:minecraft.mob_kills"),
                            "Kills", kRenderInteger, false));
  board.take_changes();

  // A display slot starts tracking, and that is the event the server turns into
  // the objective packet plus its slots and scores.
  CHECK(board.set_display_objective(kDisplaySidebar, "points"));
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kTrackingStarted, "points"));
    CHECK(!has_change(changes, ScoreboardChangeKind::kDisplayObjectiveSet, "points"));
  }
  CHECK(board.objective("points")->tracked);
  CHECK(board.display_objective_name(kDisplaySidebar) != nullptr);
  CHECK_STR(*board.display_objective_name(kDisplaySidebar), "points");
  CHECK_EQ(board.display_slot_count("points"), 1);
  // A second slot on a tracked objective is a plain display packet.
  CHECK(board.set_display_objective(kDisplayBelowName, "points"));
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kDisplayObjectiveSet, "points"));
    CHECK(!has_change(changes, ScoreboardChangeKind::kTrackingStarted, "points"));
  }
  CHECK_EQ(board.display_slot_count("points"), 2);
  // Taking one slot away while the other still shows it is another display
  // packet, with the empty name clearing the slot.
  CHECK(board.clear_display_objective(kDisplayBelowName));
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kDisplayObjectiveSet));
    CHECK(!has_change(changes, ScoreboardChangeKind::kTrackingStopped, "points"));
  }
  CHECK_EQ(board.display_slot_count("points"), 1);
  // The last slot going stops the tracking, which the server turns into the
  // objective-removed packet.
  CHECK(board.clear_display_objective(kDisplaySidebar));
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kTrackingStopped, "points"));
  CHECK(!board.objective("points")->tracked);
  CHECK(board.display_objective_name(kDisplaySidebar) == nullptr);
  CHECK(!board.clear_display_objective(kDisplaySidebar));

  // ---- scores ----
  CHECK(board.set_score("Alice", "points", 3));
  CHECK_EQ(board.score("Alice", "points"), 3);
  CHECK(board.has_score("Alice", "points"));
  CHECK(board.set_score("Bob", "points", 1));
  // Nothing was sent: the objective has no display slot at this point, so no
  // client is looking at it (`ServerScoreboard.onScoreChanged`'s tracked gate).
  CHECK(board.take_changes().empty());
  // A tracked objective sends each later change...
  CHECK(board.set_display_objective(kDisplaySidebar, "points"));
  board.take_changes();
  CHECK(board.set_score("Alice", "points", 5));
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kScoreChanged, "points", "Alice"));
  // ...and a no-op set sends nothing.
  CHECK(board.set_score("Alice", "points", 5));
  CHECK(board.take_changes().empty());
  // Adding the same value twice is one set (the second is a no-op).
  CHECK(board.add_score("Bob", "points", 4));
  CHECK_EQ(board.score("Bob", "points"), 5);
  CHECK(!board.take_changes().empty());
  // A read-only criterion refuses the command path in vanilla with
  // "Cannot modify read-only score".
  CHECK(!board.set_score("Alice", "kills", 1));
  CHECK(!board.add_score("Alice", "kills", 1));
  CHECK_EQ(board.score("Alice", "kills"), 0);
  CHECK(!board.set_score("Alice", "not_an_objective", 1));
  CHECK(board.list_scores("points").size() == 2u);
  CHECK_STR(board.list_scores("points")[0].owner, "Alice");
  CHECK_STR(board.list_scores("points")[1].owner, "Bob");
  CHECK_EQ(board.list_scores("not_an_objective").size(), 0u);
  // The lock bit is the `enable` command.
  CHECK(board.score_locked("Alice", "points"));
  CHECK(board.set_score_locked("Alice", "points", false));
  CHECK(!board.score_locked("Alice", "points"));
  CHECK(!board.set_score_locked("Alice", "points", false));
  CHECK(!board.set_score_locked("Nobody", "points", true));
  board.take_changes();

  // Resetting one score of a holder with another left is a ResetScore(owner,
  // objective); dropping the holder's last score is ResetScore(owner, null).
  CHECK(board.reset_score("Alice", "points"));
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kScoreRemoved, "points", "Alice"));
    CHECK(!has_change(changes, ScoreboardChangeKind::kPlayerScoresCleared, {}, "Alice"));
  }
  CHECK(!board.reset_score("Alice", "points"));
  CHECK(board.reset_score("Bob", "points"));
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kPlayerScoresCleared, {}, "Bob"));
  CHECK(board.set_score("Alice", "points", 1));
  board.take_changes();

  // ---- the criteria hookups ----
  // The stat branch: only objectives whose criterion is that exact stat move.
  board.add_stat_score(StatKey{kStatCustom, kCustomMobKills}, "Alice", 1);
  CHECK_EQ(board.score("Alice", "kills"), 1); // the score is created and moves
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    // Untracked: the score moved but no client is watching yet.
    CHECK(!has_change(changes, ScoreboardChangeKind::kScoreChanged, "kills", "Alice"));
  }
  board.set_display_objective(kDisplaySidebar, "kills");
  board.take_changes();
  board.add_stat_score(StatKey{kStatCustom, kCustomMobKills}, "Alice", 1);
  CHECK_EQ(board.score("Alice", "kills"), 2);
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kScoreChanged, "kills", "Alice"));
  board.add_stat_score(StatKey{kStatCustom, kCustomPlayerKills}, "Alice", 1);
  CHECK_EQ(board.score("Alice", "kills"), 2);
  // The count criteria: one increment per objective with that criterion.
  CHECK(board.add_objective("deaths", *objective_criteria_by_name("deathCount"), "Deaths",
                            kRenderInteger, false));
  board.set_display_objective(kDisplayList, "deaths");
  board.take_changes();
  board.increment_auto_criteria(ObjectiveCriteria::kAutoDeathCount, "Alice");
  CHECK_EQ(board.score("Alice", "deaths"), 1);
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kScoreChanged, "deaths", "Alice"));
  board.increment_auto_criteria(ObjectiveCriteria::kAutoTotalKillCount, "Alice");
  CHECK_EQ(board.score("Alice", "deaths"), 1);
  // The six by-value criteria.
  CHECK(
      board.add_objective("hp", *objective_criteria_by_name("health"), "HP", kRenderHearts, false));
  board.set_display_objective(kDisplayBelowName, "hp");
  board.take_changes();
  board.set_custom_criteria_score(kCriterionHealth, "Alice", 17);
  CHECK_EQ(board.score("Alice", "hp"), 17);
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kScoreChanged, "hp", "Alice"));
  board.set_custom_criteria_score(kCriterionFood, "Alice", 20);
  CHECK_EQ(board.score("Alice", "hp"), 17);

  // Removing an objective that is *on a slot and tracked* clears the slot and
  // stops the tracking, which is the objective-removed packet pair.
  CHECK(board.set_display_objective(kDisplaySidebar, "points"));
  board.take_changes();
  CHECK(board.objective("points")->tracked);
  CHECK(board.remove_objective("points"));
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kObjectiveRemoved, "points"));
    CHECK(has_change(changes, ScoreboardChangeKind::kTrackingStopped, "points"));
  }
  CHECK(board.objective("points") == nullptr);
  CHECK(board.display_objective_name(kDisplaySidebar) == nullptr);
  CHECK_EQ(board.scores("points").size(), 0u);
  CHECK(!board.remove_objective("points"));

  // ---- teams ----
  Team team;
  CHECK(board.add_team("red", &team));
  CHECK(!board.add_team("red", nullptr));
  CHECK_STR(team.name, "red");
  CHECK_STR(team.display_name, "red"); // PlayerTeam's constructor uses the name
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK(has_change(changes, ScoreboardChangeKind::kTeamAdded));
  }
  CHECK(board.join_team("Alice", "red"));
  CHECK(board.join_team("Alice", "red")); // already on it: no second event
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK_EQ(change_count(changes, ScoreboardChangeKind::kTeamPlayerJoined), 1);
  }
  CHECK(board.players_team("Alice") != nullptr);
  CHECK_STR(board.players_team("Alice")->name, "red");
  CHECK(board.players_team("Nobody") == nullptr);
  CHECK(!board.join_team("Alice", "not_a_team"));
  CHECK(board.add_team("blue", nullptr));
  board.take_changes();
  CHECK(board.join_team("Alice", "blue")); // moving emits both halves
  {
    const std::vector<ScoreboardChange> changes = board.take_changes();
    CHECK_EQ(change_count(changes, ScoreboardChangeKind::kTeamPlayerLeft), 1);
    CHECK_EQ(change_count(changes, ScoreboardChangeKind::kTeamPlayerJoined), 1);
  }
  CHECK_EQ(board.team("red")->players.size(), 0u);
  CHECK_EQ(board.team("blue")->players.size(), 1u);
  CHECK(board.leave_team("Alice"));
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kTeamPlayerLeft));
  CHECK(!board.leave_team("Alice"));
  CHECK(board.join_team("Alice", "red"));
  board.take_changes();

  CHECK(board.set_team_display_name("red", "Red Team"));
  CHECK_STR(board.team("red")->display_name, "Red Team");
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kTeamChanged));
  CHECK(board.set_team_color("red", 12 /* red */));
  CHECK(board.team("red")->has_color);
  CHECK_EQ(board.team("red")->color, 12);
  CHECK(!board.set_team_color("red", 12));
  CHECK(board.set_team_color("red", std::nullopt));
  CHECK(!board.team("red")->has_color);
  board.take_changes();
  CHECK(board.set_team_options("red", false, false));
  CHECK(!board.team("red")->allow_friendly_fire);
  CHECK(!board.team("red")->see_friendly_invisibles);
  CHECK(!board.set_team_options("red", false, false));
  CHECK(board.set_team_prefix_suffix("red", "[", "]"));
  CHECK_STR(board.team("red")->prefix, "[");
  CHECK(board.set_team_visibility("red", kVisibilityHideForOwnTeam, kVisibilityNever));
  CHECK_EQ(board.team("red")->name_tag_visibility, kVisibilityHideForOwnTeam);
  CHECK_EQ(board.team("red")->death_message_visibility, kVisibilityNever);
  CHECK(board.set_team_collision_rule("red", kCollisionPushOwnTeam));
  CHECK_EQ(board.team("red")->collision_rule, kCollisionPushOwnTeam);
  CHECK(!board.set_team_collision_rule("red", kCollisionPushOwnTeam));
  CHECK(!board.set_team_collision_rule("not_a_team", kCollisionNever));
  board.take_changes();

  // A removed team takes its members with it (the client's team packet removes
  // the whole thing).
  CHECK(board.remove_team("red"));
  CHECK(board.team("red") == nullptr);
  CHECK(board.players_team("Alice") == nullptr);
  CHECK(has_change(board.take_changes(), ScoreboardChangeKind::kTeamRemoved));
  CHECK(!board.remove_team("red"));
  CHECK(board.teams().count("blue") == 1);

  // ---- the name tables the packet and the save file both write ----
  CHECK_STR(std::string(team_color_name(0)), "black");
  CHECK_STR(std::string(team_color_name(15)), "white");
  CHECK_EQ(team_color_id("dark_aqua"), 3);
  CHECK_EQ(team_color_id("not_a_color"), -1);
  CHECK_STR(std::string(team_visibility_name(2)), "hideForOtherTeams");
  CHECK_EQ(team_visibility_id("never"), 1);
  CHECK_STR(std::string(team_collision_rule_name(3)), "pushOwnTeam");
  CHECK_EQ(team_collision_rule_id("pushOtherTeams"), 2);
  CHECK_EQ(team_visibility_id("not_visible"), -1);

  if (failures == 0)
    std::printf("scoreboard: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
