// SPDX-License-Identifier: GPL-3.0-or-later
//
// The statistics and scoreboard packets (Phase 4 milestone M), kept in a
// header so a unit test can pin every byte against the 26.2 codecs without a
// live client — the same shape entity_packets.h uses.
//
// The layouts, from the decompiled 26.2 classes:
//
//   award_stats (ClientboundAwardStatsPacket):
//     [varint count] then per stat [varint stat type][varint value id][varint count].
//     `Stat.STREAM_CODEC` dispatches on the StatType's registry id and then
//     writes the value's own registry id (item, block, entity type or
//     custom-stat), both as varints; the map's values are varints too.
//
//   set_objective (ClientboundSetObjectivePacket):
//     [utf name][byte method]; method 0 (add) and 2 (change) add
//     [Component displayName][varint render type][bool + NumberFormat].
//     The number format is absent in every packet this server sends
//     (`NumberFormatTypes.OPTIONAL_STREAM_CODEC`, one 0x00 byte), and method 1
//     (remove) stops after the byte.
//
//   set_score (ClientboundSetScorePacket):
//     [utf owner][utf objective][varint score][bool + Component display]
//     [bool + NumberFormat] — the two optional tails are absent here.
//
//   reset_score (ClientboundResetScorePacket):
//     [utf owner][bool + utf objective] (absent objective clears every score of
//     the holder).
//
//   set_display_objective (ClientboundSetDisplayObjectivePacket):
//     [varint DisplaySlot id][utf objective name], where an empty name is the
//     packet's own "no objective in this slot".
//
//   set_player_team (ClientboundSetPlayerTeamPacket):
//     [utf team name][byte method]; methods 0 (add) and 2 (change) add
//     [Component displayName][Component prefix][Component suffix]
//     [varint visibility][varint collision rule][bool + varint colour][u8 options];
//     methods 0, 3 (join) and 4 (leave) add [varint count][utf player...].
//     `packOptions` is bit 0 friendly fire, bit 1 see friendly invisibles.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "protocol/ids.h"
#include "world/scoreboard.h"
#include "world/stats.h"

namespace mc::server {

// `ClientboundSetObjectivePacket.METHOD_ADD` / `_REMOVE` / `_CHANGE`.
inline constexpr uint8_t kObjectiveMethodAdd = 0;
inline constexpr uint8_t kObjectiveMethodRemove = 1;
inline constexpr uint8_t kObjectiveMethodChange = 2;
// `ClientboundSetPlayerTeamPacket`'s methods: add, remove, change, join, leave.
inline constexpr uint8_t kTeamMethodAdd = 0;
inline constexpr uint8_t kTeamMethodRemove = 1;
inline constexpr uint8_t kTeamMethodChange = 2;
inline constexpr uint8_t kTeamMethodJoin = 3;
inline constexpr uint8_t kTeamMethodLeave = 4;

// One entry of the statistics screen's answer.
struct StatEntry {
  mc::world::StatKey stat;
  int value = 0;
};

inline std::vector<uint8_t> make_award_stats(const std::vector<StatEntry>& stats) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayAwardStatsId));
  w.varint(static_cast<uint32_t>(stats.size()));
  for (const StatEntry& entry : stats) {
    // The two halves of `Stat.STREAM_CODEC`: the type's registry id, then the
    // value's.
    w.varint(entry.stat.type);
    w.varint(static_cast<uint32_t>(entry.stat.value));
    w.varint(static_cast<uint32_t>(entry.value));
  }
  return std::move(w).take();
}

inline std::vector<uint8_t> make_set_objective(const mc::world::Objective& objective,
                                               uint8_t method) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetObjectiveId));
  w.string(objective.name);
  w.u8(method);
  if (method == kObjectiveMethodAdd || method == kObjectiveMethodChange) {
    w.component_text(objective.display_name);
    // `writeEnum` is a varint ordinal: INTEGER 0, HEARTS 1.
    w.varint(objective.render_type);
    w.u8(0); // no number format
  }
  return std::move(w).take();
}

// The same packet built from a bare name, for a caller that no longer holds an
// `Objective` (the tracking-stop burst) or wants the name as the display text.
inline std::vector<uint8_t> make_set_objective_named(const std::string& name, uint8_t method) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetObjectiveId));
  w.string(name);
  w.u8(method);
  if (method == kObjectiveMethodAdd || method == kObjectiveMethodChange) {
    // `Objective.getName()` doubles as the default display name.
    w.component_text(name);
    w.varint(0); // INTEGER
    w.u8(0);     // no number format
  }
  return std::move(w).take();
}

inline std::vector<uint8_t> make_set_score(const std::string& owner, const std::string& objective,
                                           int value) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetScoreId));
  w.string(owner);
  w.string(objective);
  w.varint(static_cast<uint32_t>(value));
  w.u8(0); // no display component
  w.u8(0); // no number format
  return std::move(w).take();
}

inline std::vector<uint8_t> make_reset_score(const std::string& owner,
                                             const std::string& objective) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlayResetScoreId));
  w.string(owner);
  if (objective.empty()) {
    w.u8(0); // no objective: every score of the holder goes
  } else {
    w.u8(1);
    w.string(objective);
  }
  return std::move(w).take();
}

inline std::vector<uint8_t> make_set_display_objective(uint8_t slot, const std::string& objective) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetDisplayObjectiveId));
  w.varint(slot);
  w.string(objective);
  return std::move(w).take();
}

// `createAddOrModifyPacket(team, create_new)` / `createRemovePacket` /
// `createPlayerPacket(team, player, action)`, with the player list each method
// carries: add sends the whole roster, join/leave send one name.
inline std::vector<uint8_t> make_set_player_team(const mc::world::Team& team, uint8_t method,
                                                 const std::string& player = {}) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(mc::protocol::kPlaySetPlayerTeamId));
  w.string(team.name);
  w.u8(method);
  if (method == kTeamMethodAdd || method == kTeamMethodChange) {
    // The packet carries the *unstyled* components: `Parameters(PlayerTeam)`
    // reads the plain getters, and the colour travels in its own field (the
    // client applies it, `PlayerTeam.applyColor`).
    w.component_text(team.display_name);
    w.component_text(team.prefix);
    w.component_text(team.suffix);
    w.varint(team.name_tag_visibility);
    w.varint(team.collision_rule);
    if (team.has_color) {
      w.u8(1);
      w.varint(team.color);
    } else {
      w.u8(0);
    }
    uint8_t options = 0;
    if (team.allow_friendly_fire)
      options |= 1;
    if (team.see_friendly_invisibles)
      options |= 2;
    w.u8(options);
  }
  if (method == kTeamMethodAdd || method == kTeamMethodJoin || method == kTeamMethodLeave) {
    if (method == kTeamMethodAdd) {
      w.varint(static_cast<uint32_t>(team.players.size()));
      for (const std::string& member : team.players)
        w.string(member);
    } else {
      w.varint(1);
      w.string(player);
    }
  }
  return std::move(w).take();
}

} // namespace mc::server
