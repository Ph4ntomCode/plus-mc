// SPDX-License-Identifier: GPL-3.0-or-later
//
// The statistics store's save file (Phase 4 milestone M's persistence half).
//
// `world/stats.h` is the model: the nine `StatType` ids, the 77 custom stats and
// `StatsCounter`. The counter lives on the player's registry entry, and that
// entry dies with the socket — so without this file every mined block, crafted
// item and killed mob is forgotten by the next boot. Vanilla's
// `ServerStatsCounter` writes `<world>/stats/<uuid>.json` from `save()` at the
// same cadence as the player data; this engine writes one `stats.dat` for the
// same reason `advancements.dat` is one file: a registry entry exists only as
// long as its session, so a store keyed by name is the only place an offline
// player's rows can live.
//
// The file is `<world>/stats.dat`: gzip NBT, one root compound, the same
// container and `DataVersion` stamp `level.dat`, `mobs.dat` and
// `advancements.dat` use. What a player's row carries:
//
//   * every non-default counter the player holds, keyed by the *criterion name*
//     `Stat.buildName` gives it (`minecraft.mined:minecraft.stone`,
//     `minecraft.custom:minecraft.play_time`). The name is the file's key
//     rather than the numeric pair so that a registry renumbering — a new item
//     inserted before an old one — cannot silently turn a stone count into a
//     dirt count. A row whose name no longer resolves is skipped, which is what
//     vanilla's own JSON read does with a stat whose type is gone.
//   * the counter's value, saturated exactly as `StatsCounter::increment`
//     saturates it.
//
// Deliberately not in the file: the dirty sets (`stats_dirty`,
// `stats_all_dirty`). They are the wire's bookkeeping, rebuilt by a join, and
// `sendStats` answers a REQUEST_STATS from the counter the file restored.
//
// The codec is pure (`nbt::Value` in and out) so the schema is unit-tested
// without the filesystem; the two file wrappers add the gzip and the atomic
// temp+rename that the other save files use.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "world/nbt.h"
#include "world/stats.h"

namespace mc::world {

// The file's name under the world root, and the DataVersion it is stamped with
// (the version `AnvilWorld::ensure_level_dat` writes level.dat with).
constexpr const char* kStatsStateFileName = "stats.dat";
constexpr int kStatsStateDataVersion = 5023;

// One player's saved counters, in `StatKey` order.
struct PlayerStatsState {
  std::string player;
  std::vector<std::pair<StatKey, int>> stats;
};

// The whole file: one row per player who has at least one saved counter.
struct StatsState {
  std::vector<PlayerStatsState> players;
  bool empty() const {
    return players.empty();
  }
};

// The store as the file's root compound:
//
//   root {
//     DataVersion int 5023
//     Players     list<compound>, one per player with a counter {
//       Name  string            the login name (the store's key)
//       Stats list<compound>, sorted by criterion name {
//         Name  string          Stat.buildName's criterion name
//         Value int             StatsCounter's value
//       }
//     }
//   }
//
// A player with no counters is skipped, and so is a stat whose criterion name
// no longer resolves: a row that restores nothing is not written.
nbt::Value stats_state_to_nbt(const StatsState& state);

// Reads that compound back. A player row with no name, a stat row with no name
// or value, or a stat name this build's registries cannot resolve is skipped
// (with a note in the log); a row whose own list field (`Stats`) is present but
// not the list it must be skips its player. What rejects the whole read is a
// root that is not this file's shape at all, `Players` included; `out` is
// untouched on a rejection.
bool stats_state_from_nbt(const nbt::Value& root, StatsState* out);

// Writes `<world_dir>/stats.dat` (gzip, temp+rename: a crash never leaves a
// torn file). An absent directory is an error. Returns false on any I/O error.
bool write_stats_state(const std::filesystem::path& world_dir, const StatsState& state);

// Reads `<world_dir>/stats.dat`. A *missing* file is success with an empty
// state (a fresh world, or one that never had a player); a present file that
// cannot be read, inflated or parsed is a failure. On failure `out` is
// untouched.
bool read_stats_state(const std::filesystem::path& world_dir, StatsState* out);

} // namespace mc::world
