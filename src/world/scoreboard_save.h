// SPDX-License-Identifier: GPL-3.0-or-later
//
// The scoreboard's save file (Phase 4 milestone M's persistence half).
//
// `world/scoreboard.h` is the model: objectives, their criteria, every score,
// the display slots and the teams. It is one server-wide object
// (`MinecraftServer.getScoreboard`), and nothing about it survives a restart
// today — an operator's sidebar and a survival server's death counter are gone
// by the next boot. Vanilla writes all of it to `<world>/data/scoreboard.dat`
// (`ScoreboardSaveData.save`), and this is that file's engine-side twin:
// `<world>/scoreboard.dat`, gzip NBT, one root compound, the same container and
// `DataVersion` stamp the other save files use.
//
// The schema follows vanilla's own `ScoreboardSaveData` field names, because
// the content is the same content:
//
//   root {
//     DataVersion int 5023
//     Objectives  list<compound> {
//       Name              string   the objective's key
//       CriteriaName      string   `ObjectiveCriteria.byName`'s name
//       DisplayName       string   the literal text this engine renders
//       RenderType        string   "integer" | "hearts"
//       DisplayAutoUpdate byte
//     }
//     PlayerScores list<compound> {
//       Name      string   the score holder
//       Objective string
//       Score     int
//       Locked    byte     Score.locked
//     }
//     DisplaySlots compound {
//       list | sidebar | belowName | sidebar.team.<colour> -> objective name
//     }
//     Teams list<compound> {
//       Name, DisplayName, Color (int, -1 = none), AllowFriendlyFire,
//       SeeFriendlyInvisibles, NameTagVisibility, DeathMessageVisibility,
//       CollisionRule, Prefix, Suffix, Players (list<string>)
//     }
//   }
//
// Two differences from vanilla's file are deliberate and documented: display
// names, prefixes and suffixes are plain strings where vanilla carries a JSON
// `Component` (the model's own limitation, see world/scoreboard.h), and the
// display slots use the names above rather than vanilla's raw byte keys. Both
// keep the file readable and round-trip exactly through this engine.
//
// The read restores a *fresh* `Scoreboard`: objectives first, then their
// scores (through `restore_score`, which skips the read-only check because a
// stat objective's scores are exactly what the file carries), then the display
// slots, then the teams and their members. A row that cannot be reconstructed —
// an unknown criteria name, a score on an unknown objective, a slot naming an
// objective the file did not carry — is skipped with a log line; only a root
// that is not this file's shape at all is rejected.
#pragma once

#include <filesystem>

#include "world/nbt.h"
#include "world/scoreboard.h"

namespace mc::world {

// The file's name under the world root, and the DataVersion it is stamped with.
constexpr const char* kScoreboardStateFileName = "scoreboard.dat";
constexpr int kScoreboardStateDataVersion = 5023;

// The scoreboard as the file's root compound (schema in the header comment).
nbt::Value scoreboard_state_to_nbt(const Scoreboard& scoreboard);

// Reads that compound into `out`, which is replaced on success and untouched on
// rejection. See the header comment for the rows a read skips.
bool scoreboard_state_from_nbt(const nbt::Value& root, Scoreboard* out);

// Writes `<world_dir>/scoreboard.dat` (gzip, temp+rename). An absent directory
// is an error. Returns false on any I/O error.
bool write_scoreboard_state(const std::filesystem::path& world_dir, const Scoreboard& scoreboard);

// Reads `<world_dir>/scoreboard.dat`. A *missing* file is success with an empty
// board (a fresh world); a present file that cannot be read, inflated or parsed
// is a failure. On failure `out` is untouched.
bool read_scoreboard_state(const std::filesystem::path& world_dir, Scoreboard* out);

} // namespace mc::world
