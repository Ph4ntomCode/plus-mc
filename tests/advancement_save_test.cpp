// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the advancement store's save codec (world/advancement_save.h)
// and the model's half of it (`PlayerAdvancements::saved_progress` /
// `apply_saved_progress`), the pair that makes a player's tree survive a
// restart.
//
// What it pins:
//   * `saved_progress` keeps exactly the advancements `hasProgress` sees: the
//     empty entries a visibility walk starts are not rows, only obtained
//     criteria are written, and the rows come out sorted by id (a stable file);
//   * a state round-trips through `nbt::serialize` -> `nbt::parse`: ids,
//     criterion names and their obtained instants, and the known recipes;
//   * a fresh model that adopts the rows is the model that wrote them — same
//     `saved_progress`, the completed advancement done, the open partner still
//     partial, the file's instant (not the reload's), an empty reward queue (a
//     reload must not re-pay rewards), and a first flush that resets the
//     client's tree and carries the restored rows;
//   * the reader's skip rules (an unnamed player, an id-less advancement, a
//     nameless criterion, a player with nothing to remember, a row's
//     `Obtained` that is not a number) and its rejection of a root that is not
//     this file's shape at all, with `out` untouched;
//   * the writer writes no empty row, and `apply_saved_progress` drops an
//     advancement id this build's table no longer carries.
#include <cstdio>
#include <string>
#include <vector>

#include "world/advancement.h"
#include "world/advancement_save.h"
#include "world/nbt.h"

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

const SavedAdvancementProgress* find_row(const std::vector<SavedAdvancementProgress>& rows,
                                         const char* id) {
  for (const SavedAdvancementProgress& row : rows)
    if (row.advancement == id)
      return &row;
  return nullptr;
}

// Row-for-row equality: the ids, the criteria and their instants. This is what
// "a restart does not drift" means in bytes.
bool same_rows(const std::vector<SavedAdvancementProgress>& a,
               const std::vector<SavedAdvancementProgress>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].advancement != b[i].advancement || a[i].criteria.size() != b[i].criteria.size())
      return false;
    for (size_t c = 0; c < a[i].criteria.size(); ++c)
      if (a[i].criteria[c].criterion != b[i].criteria[c].criterion ||
          a[i].criteria[c].obtained_ms != b[i].criteria[c].obtained_ms)
        return false;
  }
  return true;
}

} // namespace

int main() {
  CHECK_STR(kAdvancementStateFileName, "advancements.dat");
  CHECK_EQ(kAdvancementStateDataVersion, 5023);

  const GeneratedAdvancement* story_root = advancement_by_id("minecraft:story/root");
  const GeneratedAdvancement* trim_open =
      advancement_by_id("minecraft:adventure/trim_with_all_exclusive_armor_patterns");
  const GeneratedAdvancement* trim_done =
      advancement_by_id("minecraft:adventure/trim_with_any_armor_pattern");
  const GeneratedAdvancement* bred_open = advancement_by_id("minecraft:husbandry/bred_all_animals");
  const GeneratedAdvancement* bred_done = advancement_by_id("minecraft:husbandry/breed_an_animal");
  CHECK(story_root != nullptr && trim_open != nullptr && trim_done != nullptr &&
        bred_open != nullptr && bred_done != nullptr);
  if (failures != 0)
    return 1;

  // ---- the model's half: what a save keeps ---------------------------------
  PlayerAdvancements advancements;
  // A direct grant with a time of its own: the save must carry the instant, not
  // the moment of the reload.
  CHECK(advancements.award(*story_root, "crafting_table", 1234567));
  CHECK(advancements.is_done(*story_root));
  // Two events that leave one advancement done and its partner open: a trim's
  // craft names a criterion in each trim advancement, a birth names the
  // condition-less `bred` and one of `bred_all_animals`' 26 per-animal ones.
  CHECK_EQ(advancements.handle_recipe_crafted(
               "minecraft:rib_armor_trim_smithing_template_smithing_trim"),
           2u);
  CHECK(advancements.is_done(*trim_done));
  CHECK(!advancements.is_done(*trim_open));
  CHECK_EQ(advancements.handle_bred_animals("minecraft:cow", "minecraft:cow", "minecraft:cow"), 2u);
  CHECK(advancements.is_done(*bred_done));
  CHECK(!advancements.is_done(*bred_open));
  // A flush walks the visible trees and starts an empty progress entry for
  // every node it visits, so the rows below are the `hasProgress` filter's work
  // and not just the handlers'.
  const PlayerAdvancements::Update first = advancements.flush(true);
  CHECK_EQ(first.reset, true);
  CHECK(!first.progress.empty());

  const std::vector<SavedAdvancementProgress> rows = advancements.saved_progress();
  size_t with_progress = 0;
  for (int i = 0; i < advancement_count(); ++i) {
    const AdvancementProgress* progress = advancements.progress_of(*advancement_at(i));
    if (progress != nullptr && progress->has_progress())
      ++with_progress;
  }
  // The model agrees with itself: one row per advancement with progress.
  CHECK_EQ(rows.size(), with_progress);
  // ...and the five the events above moved: two done roots' descendants, the
  // trim pair and the breeding pair.
  CHECK_EQ(rows.size(), 5u);
  const SavedAdvancementProgress* root_row = find_row(rows, "minecraft:story/root");
  CHECK(root_row != nullptr);
  if (root_row != nullptr) {
    CHECK_EQ(root_row->criteria.size(), 1u);
    CHECK_STR(root_row->criteria[0].criterion, "crafting_table");
    CHECK_EQ(root_row->criteria[0].obtained_ms, 1234567);
  }
  const SavedAdvancementProgress* open_row =
      find_row(rows, "minecraft:adventure/trim_with_all_exclusive_armor_patterns");
  CHECK(open_row != nullptr);
  if (open_row != nullptr) {
    // The open advancement's one criterion is a row all the same: it is
    // progress, and it is what keeps the rejoin from showing `0/8`.
    CHECK_EQ(open_row->criteria.size(), 1u);
    CHECK_STR(open_row->criteria[0].criterion,
              "armor_trimmed_minecraft:rib_armor_trim_smithing_template_smithing_trim");
  }
  // Rows are sorted by id: `sorted_progress` is `progress_`'s hash order, so the
  // file's bytes must not come from it.
  for (size_t i = 1; i < rows.size(); ++i)
    CHECK(rows[i - 1].advancement < rows[i].advancement);

  // ---- the file's half: a state round-trips --------------------------------
  AdvancementState state;
  state.players.push_back(
      PlayerAdvancementState{"Alice", rows, {"minecraft:crafting_table", "minecraft:stick"}});
  state.players.push_back(PlayerAdvancementState{"Bob", {}, {"minecraft:oak_planks"}});
  const nbt::Value file = advancement_state_to_nbt(state);
  CHECK_EQ(nbt::as_int(*nbt::child(file, "DataVersion")).value_or(-1),
           kAdvancementStateDataVersion);
  const nbt::Value* players = nbt::child(file, "Players");
  CHECK(players != nullptr && players->type == nbt::Tag::List && players->items.size() == 2u);
  const std::vector<uint8_t> bytes = nbt::serialize(file);
  const std::optional<nbt::Value> parsed = nbt::parse(bytes.data(), bytes.size());
  CHECK(parsed.has_value());
  AdvancementState back;
  CHECK(advancement_state_from_nbt(*parsed, &back));
  CHECK_EQ(back.players.size(), 2u);
  if (back.players.size() == 2u) {
    CHECK_STR(back.players[0].player, "Alice");
    CHECK(same_rows(back.players[0].progress, rows));
    CHECK_EQ(back.players[0].known_recipes.size(), 2u);
    CHECK_STR(back.players[0].known_recipes[1], "minecraft:stick");
    CHECK_STR(back.players[1].player, "Bob");
    CHECK(back.players[1].progress.empty());
    CHECK_EQ(back.players[1].known_recipes.size(), 1u);
    // The parsed file is the file: serializing it again is the same bytes.
    CHECK_EQ(nbt::as_int(*nbt::child(advancement_state_to_nbt(back), "DataVersion")).value_or(-1),
             kAdvancementStateDataVersion);
  }

  // ---- the reload: a fresh model adopts the rows ---------------------------
  // The parsed rows when the round trip above held, the model's own rows
  // otherwise (it failed loudly there already).
  const std::vector<SavedAdvancementProgress>& round_tripped =
      back.players.size() == 2u ? back.players[0].progress : rows;
  PlayerAdvancements reloaded;
  const std::size_t adopted = reloaded.apply_saved_progress(round_tripped);
  CHECK_EQ(adopted, rows.size());
  // A reload restores progress; it does not re-pay what the advancement's
  // completion already paid (the session drains this list for the rewards).
  CHECK(reloaded.completed().empty());
  CHECK(reloaded.is_done(*story_root));
  CHECK(reloaded.is_done(*trim_done));
  CHECK(reloaded.is_done(*bred_done));
  CHECK(!reloaded.is_done(*trim_open));
  CHECK(!reloaded.is_done(*bred_open));
  const AdvancementProgress* root_progress = reloaded.progress_of(*story_root);
  CHECK(root_progress != nullptr);
  if (root_progress != nullptr) {
    CHECK(root_progress->criterion_done("crafting_table"));
    CHECK_EQ(root_progress->criteria().at("crafting_table").obtained_ms, 1234567);
  }
  // The visibility walk comes back with the progress: the first flush after the
  // reload resets the client's stale tree and carries the restored rows.
  const PlayerAdvancements::Update rejoin = reloaded.flush(true);
  CHECK_EQ(rejoin.reset, true);
  CHECK(!rejoin.added.empty());
  CHECK(!rejoin.progress.empty());
  bool carried_root = false;
  for (const auto& [advancement, progress] : rejoin.progress)
    if (std::string(advancement->id) == "minecraft:story/root")
      carried_root = true;
  CHECK(carried_root);
  // ...and a save of the reloaded model writes the same rows: a restart cycle
  // does not drift.
  CHECK(same_rows(reloaded.saved_progress(), rows));

  // ---- the reader's skip rules --------------------------------------------
  {
    AdvancementState untouched;
    untouched.players.push_back(PlayerAdvancementState{"keep", {}, {}});
    // A root that is not this file's shape at all is rejected, `out` untouched.
    CHECK(!advancement_state_from_nbt(nbt::make_int(3), &untouched));
    CHECK_EQ(untouched.players.size(), 1u);
    CHECK(!advancement_state_from_nbt(
        nbt::make_compound({{"Players", nbt::make_list(nbt::Tag::Int, {nbt::make_int(1)})}}),
        &untouched));
    CHECK_EQ(untouched.players.size(), 1u);

    // A well-shaped file whose rows are partly broken keeps the good parts: the
    // unnamed player, the id-less advancement and the nameless criterion are
    // dropped, and an `Obtained` that is not a number reads as the epoch. Built
    // row by row rather than as one literal: the nesting is four deep.
    const nbt::Value stick_recipe =
        nbt::make_list(nbt::Tag::String, {nbt::make_string("minecraft:stick")});
    const nbt::Value nameless_player = // no Name at all: dropped
        nbt::make_compound({{"Recipes", stick_recipe}});
    const nbt::Value empty_name = // an empty name: dropped
        nbt::make_compound({{"Name", nbt::make_string("")}});
    const nbt::Value nameless_criterion = nbt::make_compound({{"Obtained", nbt::make_long(5)}});
    const nbt::Value text_time = // not a number: kept, read as 0
        nbt::make_compound(
            {{"Name", nbt::make_string("crafting_table")}, {"Obtained", nbt::make_string("nope")}});
    const nbt::Value criterion_x = nbt::make_compound({{"Name", nbt::make_string("x")}});
    const nbt::Value id_less = // the advancement row itself has no Id: dropped
        nbt::make_compound({{"Criteria", nbt::make_list(nbt::Tag::Compound, {criterion_x})}});
    const nbt::Value story_root_row = nbt::make_compound(
        {{"Id", nbt::make_string("minecraft:story/root")},
         {"Criteria", nbt::make_list(nbt::Tag::Compound, {nameless_criterion, text_time})}});
    const nbt::Value carol = nbt::make_compound(
        {{"Name", nbt::make_string("Carol")},
         {"Advancements", nbt::make_list(nbt::Tag::Compound, {id_less, story_root_row})}});
    const nbt::Value nobody = // nothing to remember: dropped
        nbt::make_compound({{"Name", nbt::make_string("Dave")}});
    const nbt::Value mixed = nbt::make_compound(
        {{"Players",
          nbt::make_list(nbt::Tag::Compound, {nameless_player, empty_name, carol, nobody})}});
    AdvancementState broken;
    CHECK(advancement_state_from_nbt(mixed, &broken));
    CHECK_EQ(broken.players.size(), 1u);
    if (broken.players.size() == 1u) {
      CHECK_STR(broken.players[0].player, "Carol");
      CHECK_EQ(broken.players[0].progress.size(), 1u);
      if (broken.players[0].progress.size() == 1u) {
        CHECK_STR(broken.players[0].progress[0].advancement, "minecraft:story/root");
        CHECK_EQ(broken.players[0].progress[0].criteria.size(), 1u);
        CHECK_EQ(broken.players[0].progress[0].criteria[0].obtained_ms, 0);
      }
      CHECK(broken.players[0].known_recipes.empty());
    }
  }

  // An advancement id the generated table no longer carries is dropped by the
  // model, and the rows it does know are still adopted.
  {
    PlayerAdvancements fresh;
    const std::vector<SavedAdvancementProgress> mixed = {
        SavedAdvancementProgress{"minecraft:not/an_advancement", {SavedCriterion{"x", 1}}},
        SavedAdvancementProgress{"minecraft:story/root", {SavedCriterion{"crafting_table", 99}}},
    };
    CHECK_EQ(fresh.apply_saved_progress(mixed), 1u);
    CHECK(fresh.is_done(*story_root));
  }

  // The writer writes no empty row: a player with nothing to remember and an
  // advancement with no obtained criteria are not in the file.
  {
    AdvancementState sparse;
    sparse.players.push_back(PlayerAdvancementState{"Nobody", {}, {}});
    sparse.players.push_back(PlayerAdvancementState{
        "Somebody", {SavedAdvancementProgress{"minecraft:story/root", {}}}, {"minecraft:stick"}});
    const nbt::Value sparse_file = advancement_state_to_nbt(sparse);
    const nbt::Value* sparse_players = nbt::child(sparse_file, "Players");
    CHECK(sparse_players != nullptr && sparse_players->items.size() == 1u);
    if (sparse_players != nullptr && sparse_players->items.size() == 1u) {
      const nbt::Value* name = nbt::child(sparse_players->items[0], "Name");
      CHECK_STR(std::string(nbt::as_string(*name).value_or("")), "Somebody");
      const nbt::Value* advancement_rows = nbt::child(sparse_players->items[0], "Advancements");
      CHECK(advancement_rows != nullptr && advancement_rows->items.empty());
    }
  }

  // An empty store is an empty file, and reading an empty file is an empty
  // store: a fresh world's boot is not a special case.
  {
    const nbt::Value empty_file = advancement_state_to_nbt(AdvancementState{});
    const nbt::Value* empty_players = nbt::child(empty_file, "Players");
    CHECK(empty_players != nullptr && empty_players->items.empty());
    AdvancementState empty_back;
    CHECK(advancement_state_from_nbt(empty_file, &empty_back));
    CHECK(empty_back.empty());
  }

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("advancement_save: all checks passed\n");
  return 0;
}
