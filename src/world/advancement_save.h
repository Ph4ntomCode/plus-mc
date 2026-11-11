// SPDX-License-Identifier: GPL-3.0-or-later
//
// The advancement store's save file (Phase 4 milestone N): the progress that
// survives a restart, the way mobs.dat's entity ids and animal state do.
//
// Vanilla keeps one JSON file per player under `<world>/advancements/<uuid>.json`
// — `PlayerAdvancements.save` on every save, read back by the `ServerPlayer`
// constructor (`load` -> `applyFrom`, which is what marks the loaded rows dirty
// and the roots for the visibility walk). Three things about this engine move
// the file's shape:
//
//   * players are named, not uuid-keyed (the registry entry carries the login
//     name; docs/PHASE2.md), so a row is keyed by name;
//   * a registry entry lives only as long as its session, so the store must be
//     one process-wide file rather than one file per live player — an offline
//     player's rows are still rows;
//   * the mob population already has a gzip-NBT save file per world
//     (world/mob_save.h), so this one is too.
//
// So the file is `<world>/advancements.dat`: gzip NBT, one root compound with
// one row per player who has something to remember. What a row carries:
//
//   * the advancement progress: `PlayerAdvancements.asData`'s rows — one per
//     advancement with any criterion obtained — and inside a row the criteria
//     map of `AdvancementProgress.CODEC`: only the obtained criteria, each with
//     the instant it was obtained. Every row the file keeps is a row the first
//     `update_advancements` after a join sends back (as `progress` under a
//     `reset` packet, which is what drops the client's stale tree).
//   * `ServerRecipeBook.known` (`Player::known_recipes`): this engine's recipe
//     book is global, so the set gates no packet — but it is the
//     `minecraft:recipe_unlocked` trigger's one-shot rule, and without it in
//     the file every recipe a player had already learned would fire its
//     trigger again on the first unlock event after a restart.
//
// What is deliberately *not* in the file: the selected tab (vanilla's
// `lastSelectedTab` is session state — a rejoining client's screen is closed),
// the dirty sets and the first-packet flag (rebuilt by the reload, which is
// exactly what makes the first flush after a join reset the client's tree), and
// anything only a criterion this engine cannot raise would need (location
// triggers, `tame_animal`): a player's progress there does not exist in the
// first place. As with mobs.dat, a vanilla server does not read this file, and
// this server does not read vanilla's `advancements/` folder.
//
// The codec is pure (`nbt::Value` in and out) so the schema is unit-tested
// without the filesystem; the two file wrappers add the gzip and the atomic
// temp+rename that level.dat's writer uses.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "world/advancement.h"
#include "world/nbt.h"

namespace mc::world {

// The file's name under the world root, and the DataVersion it is stamped with
// (the version `AnvilWorld::ensure_level_dat` writes level.dat with, the same
// stamp mobs.dat carries).
constexpr const char* kAdvancementStateFileName = "advancements.dat";
constexpr int kAdvancementStateDataVersion = 5023;

// One player's saved state — what a restart would otherwise drop.
struct PlayerAdvancementState {
  std::string player;
  // `asData`'s rows (see PlayerAdvancements::saved_progress), in id order.
  std::vector<SavedAdvancementProgress> progress;
  // `ServerRecipeBook.known`, sorted (a std::set's iteration order).
  std::vector<std::string> known_recipes;
};

// The whole file: one row per player who has progress or known recipes. A
// player with neither has no row (vanilla writes them an empty `{}`).
struct AdvancementState {
  std::vector<PlayerAdvancementState> players;
  bool empty() const {
    return players.empty();
  }
};

// The store as the file's root compound:
//
//   root {
//     DataVersion  int  5023
//     Players      list<compound>, one per player with something to remember {
//       Name         string            the login name (the store's key)
//       Recipes      list<string>      Player::known_recipes, sorted
//       Advancements list<compound>, one per advancement with any criterion {
//         Id       string              the advancement id the table uses
//         Criteria list<compound> {
//           Name     string            the criterion's own name
//           Obtained long              epoch milliseconds (Instant on the wire)
//         }
//       }
//     }
//   }
//
// A player with no progress and no known recipes is skipped, and so is a row
// with no criteria left: a row with nothing obtained restores nothing.
nbt::Value advancement_state_to_nbt(const AdvancementState& state);

// Reads that compound back. A player row with no name, an advancement row with
// no id or a criterion row with no name is skipped (with a note in the log) —
// and so is a row whose own list field (`Recipes`, `Advancements`, `Criteria`)
// is present but not the list it must be. What rejects the whole read is a root
// that is not this file's shape at all, `Players` included; `out` is untouched
// on a rejection.
bool advancement_state_from_nbt(const nbt::Value& root, AdvancementState* out);

// Writes `<world_dir>/advancements.dat` (gzip, temp+rename: a crash never
// leaves a torn file). An absent directory is an error. Returns false on any
// I/O error.
bool write_advancement_state(const std::filesystem::path& world_dir, const AdvancementState& state);

// Reads `<world_dir>/advancements.dat`. A *missing* file is success with an
// empty state (a fresh world, or one that never had a player); a present file
// that cannot be read, inflated or parsed is a failure. On failure `out` is
// untouched.
bool read_advancement_state(const std::filesystem::path& world_dir, AdvancementState* out);

} // namespace mc::world
