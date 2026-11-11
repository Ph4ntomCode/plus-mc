// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test: the scoreboard's save codec (world/scoreboard_save.{h,cpp}).
//
// What it pins:
//   * objectives round-trip with their criteria, display name, render type and
//     display-auto-update flag; scores round-trip with their values and locks,
//     including a *read-only* stat criterion's scores, which the command path
//     refuses but the load path (`restore_score`) must restore;
//   * every display slot survives, including a team-colour sidebar;
//   * teams round-trip with colour, both option flags, visibility, collision
//     rule, prefix/suffix and members;
//   * the restore's change log is empty (a load is not a mutation a client
//     hears about);
//   * a row whose criteria name this build cannot resolve, a score on an
//     unknown objective and a slot naming an unknown objective are skipped, and
//     a root that is not this file's shape is rejected with `out` untouched.
#include <cstdio>
#include <string>

#include "world/nbt.h"
#include "world/scoreboard.h"
#include "world/scoreboard_save.h"
#include "world/stats.h"

using namespace mc::world;
namespace nbt = mc::world::nbt;

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

int main() {
  const std::string play_time_criterion =
      stat_criterion_name(StatKey{kStatCustom, kCustomPlayTime});
  CHECK_STR(play_time_criterion, "minecraft.custom:minecraft.play_time");
  const std::optional<ObjectiveCriteria> dummy = objective_criteria_by_name("dummy");
  const std::optional<ObjectiveCriteria> stat_criteria =
      objective_criteria_by_name(play_time_criterion);
  CHECK(dummy.has_value());
  CHECK(stat_criteria.has_value());
  if (!dummy || !stat_criteria)
    return 1;

  // ---- the round trip -------------------------------------------------------
  Scoreboard board;
  CHECK(board.add_objective("points", *dummy, "Points", kRenderInteger, true));
  CHECK(board.set_score("Alice", "points", 7));
  CHECK(board.set_score_locked("Alice", "points", false));
  CHECK(board.set_score("Bob", "points", 0));
  // The read-only criterion a command cannot write is exactly what the file
  // carries: `restore_score` bypasses the check.
  CHECK(board.add_objective("playtime", *stat_criteria, "Time Played", kRenderInteger, false));
  CHECK(board.restore_score("Alice", "playtime", 72000, true));
  CHECK(board.set_display_objective(kDisplaySidebar, "points"));
  CHECK(board.set_display_objective(kDisplayTeamColorFirst + 4, "playtime"));

  CHECK(board.add_team("red", nullptr));
  CHECK(board.set_team_display_name("red", "Red Team"));
  CHECK(board.set_team_color("red", 4));
  CHECK(board.set_team_options("red", false, true));
  CHECK(board.set_team_prefix_suffix("red", "[R] ", "!"));
  CHECK(board.set_team_visibility("red", kVisibilityHideForOtherTeams, kVisibilityNever));
  CHECK(board.set_team_collision_rule("red", kCollisionPushOwnTeam));
  CHECK(board.join_team("Alice", "red"));
  CHECK(board.join_team("Bob", "red"));
  board.take_changes();

  const nbt::Value root = scoreboard_state_to_nbt(board);
  const std::vector<uint8_t> bytes = nbt::serialize(root);
  const std::optional<nbt::Value> parsed = nbt::parse(bytes.data(), bytes.size());
  CHECK(parsed.has_value());
  Scoreboard loaded;
  CHECK(scoreboard_state_from_nbt(*parsed, &loaded));
  CHECK_EQ(loaded.objectives().size(), 2);
  CHECK_EQ(loaded.teams().size(), 1);
  const Objective* points = loaded.objective("points");
  CHECK(points != nullptr);
  if (points != nullptr) {
    CHECK_STR(points->criteria.name, "dummy");
    CHECK_STR(points->display_name, "Points");
    CHECK_EQ(points->render_type, kRenderInteger);
    CHECK(points->display_auto_update);
    CHECK(points->tracked);
  }
  const Objective* playtime = loaded.objective("playtime");
  CHECK(playtime != nullptr);
  if (playtime != nullptr) {
    CHECK(playtime->criteria.read_only);
    CHECK(playtime->criteria.is_stat);
    CHECK(playtime->criteria.stat == (StatKey{kStatCustom, kCustomPlayTime}));
    CHECK_STR(playtime->display_name, "Time Played");
    CHECK(!playtime->display_auto_update);
  }
  CHECK_EQ(loaded.score("Alice", "points"), 7);
  CHECK(!loaded.score_locked("Alice", "points"));
  CHECK_EQ(loaded.score("Bob", "points"), 0);
  CHECK_EQ(loaded.score("Alice", "playtime"), 72000);
  CHECK(loaded.score_locked("Alice", "playtime"));
  const std::string* sidebar = loaded.display_objective_name(kDisplaySidebar);
  CHECK(sidebar != nullptr && *sidebar == "points");
  const std::string* team_slot = loaded.display_objective_name(kDisplayTeamColorFirst + 4);
  CHECK(team_slot != nullptr && *team_slot == "playtime");
  CHECK(loaded.display_objective_name(kDisplayList) == nullptr);

  const Team* loaded_red = loaded.team("red");
  CHECK(loaded_red != nullptr);
  if (loaded_red != nullptr) {
    CHECK_STR(loaded_red->display_name, "Red Team");
    CHECK(loaded_red->has_color);
    CHECK_EQ(loaded_red->color, 4);
    CHECK(!loaded_red->allow_friendly_fire);
    CHECK(loaded_red->see_friendly_invisibles);
    CHECK_STR(loaded_red->prefix, "[R] ");
    CHECK_STR(loaded_red->suffix, "!");
    CHECK_EQ(loaded_red->name_tag_visibility, kVisibilityHideForOtherTeams);
    CHECK_EQ(loaded_red->death_message_visibility, kVisibilityNever);
    CHECK_EQ(loaded_red->collision_rule, kCollisionPushOwnTeam);
    CHECK_EQ(loaded_red->players.size(), 2);
    CHECK(loaded.players_team("Alice") == loaded_red);
    CHECK(loaded.players_team("Bob") == loaded_red);
  }
  // A load restored the board, it did not "change" it.
  CHECK_EQ(loaded.take_changes().size(), 0);

  // ---- rows a reader must skip ---------------------------------------------
  const nbt::Value with_unknown = nbt::make_compound({
      {"DataVersion", nbt::make_int(kScoreboardStateDataVersion)},
      {"Objectives",
       nbt::make_list(nbt::Tag::Compound,
                      {nbt::make_compound({
                           {"Name", nbt::make_string("bad")},
                           {"CriteriaName", nbt::make_string("minecraft:not_a_criterion")},
                           {"DisplayName", nbt::make_string("Bad")},
                           {"RenderType", nbt::make_string("integer")},
                       }),
                       nbt::make_compound({
                           {"Name", nbt::make_string("ok")},
                           {"CriteriaName", nbt::make_string("dummy")},
                           {"DisplayName", nbt::make_string("OK")},
                           {"RenderType", nbt::make_string("hearts")},
                       })})},
      {"PlayerScores",
       nbt::make_list(nbt::Tag::Compound, {nbt::make_compound({
                                               {"Name", nbt::make_string("Alice")},
                                               {"Objective", nbt::make_string("bad")},
                                               {"Score", nbt::make_int(1)},
                                           }),
                                           nbt::make_compound({
                                               {"Name", nbt::make_string("Alice")},
                                               {"Objective", nbt::make_string("ok")},
                                               {"Score", nbt::make_int(5)},
                                           })})},
      {"DisplaySlots", nbt::make_compound({{"sidebar", nbt::make_string("bad")},
                                           {"belowName", nbt::make_string("ok")},
                                           {"sidebar.team.purple", nbt::make_string("ok")}})},
      {"Teams", nbt::make_list(nbt::Tag::Compound, {})},
  });
  Scoreboard pruned;
  CHECK(scoreboard_state_from_nbt(with_unknown, &pruned));
  CHECK_EQ(pruned.objectives().size(), 1);
  CHECK(pruned.objective("bad") == nullptr);
  const Objective* ok = pruned.objective("ok");
  CHECK(ok != nullptr);
  if (ok != nullptr)
    CHECK_EQ(ok->render_type, kRenderHearts);
  CHECK_EQ(pruned.score("Alice", "ok"), 5);
  CHECK(pruned.display_objective_name(kDisplaySidebar) == nullptr);
  CHECK(pruned.display_objective_name(kDisplayBelowName) != nullptr);
  CHECK(pruned.display_objective_name(kDisplayTeamColorFirst + 13) == nullptr); // purple = 13

  // A root that is not this file's shape is rejected and leaves `out` alone.
  const size_t before = loaded.objectives().size();
  CHECK(!scoreboard_state_from_nbt(nbt::make_string("nope"), &loaded));
  CHECK_EQ(loaded.objectives().size(), before);
  const nbt::Value bad_objectives = nbt::make_compound({
      {"Objectives", nbt::make_string("not a list")},
  });
  CHECK(!scoreboard_state_from_nbt(bad_objectives, &loaded));
  CHECK_EQ(loaded.objectives().size(), before);

  if (failures == 0)
    std::printf("scoreboard_save: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
