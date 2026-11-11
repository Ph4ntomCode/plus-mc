// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 4 milestone M: the statistics and scoreboard packets, pinned
// byte-for-byte against the vanilla 26.2 codecs (ClientboundAwardStatsPacket,
// ClientboundSetObjectivePacket, ClientboundSetScorePacket,
// ClientboundResetScorePacket, ClientboundSetDisplayObjectivePacket,
// ClientboundSetPlayerTeamPacket). Pure logic, no sockets — the same shape
// tests/drop_packet_test.cpp gives the entity packets.
//
// The layouts this guards, each one field away from a misparse:
//
//   * award_stats     [varint count]{[varint stat type][varint value id]
//                     [varint count]}
//   * set_objective   [utf name][byte method], then (add/change only)
//                     [Component display][varint render type]
//                     [bool + NumberFormat]
//   * set_score       [utf owner][utf objective][varint score]
//                     [bool + Component display][bool + NumberFormat]
//   * reset_score     [utf owner][bool + utf objective]
//   * set_display     [varint DisplaySlot id][utf objective name]
//   * set_player_team [utf name][byte method], then (add/change only)
//                     [Component display][prefix][suffix][varint visibility]
//                     [varint collision][bool + varint colour][u8 options],
//                     then (add/join/leave only) [varint count][utf player...]
//
// A Component with no styling is a bare TAG_String: 0x08, a u16 length, then
// the bytes (net/byte_buffer.h's `component_text`). The absent optionals are
// one 0x00 byte each. Every int on the wire is a varint, so a negative score
// is the five-byte -1.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "server/scoreboard_packets.h"

namespace {

int g_failures = 0;

std::string hex(const std::vector<uint8_t>& b) {
  static const char* k = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (uint8_t c : b) {
    s.push_back(k[c >> 4]);
    s.push_back(k[c & 0x0F]);
  }
  return s;
}

void expect_bytes(const char* what, const std::vector<uint8_t>& got,
                  const std::vector<uint8_t>& want) {
  if (got == want)
    return;
  ++g_failures;
  std::printf("FAIL: %s\n  got  %s\n  want %s\n", what, hex(got).c_str(), hex(want).c_str());
}

// TAG_String: the literal component every unstyled display name/prefix/suffix
// is written as.
void push_string_tag(std::vector<uint8_t>* out, const std::string& text) {
  out->push_back(0x08);
  out->push_back(static_cast<uint8_t>(text.size() >> 8));
  out->push_back(static_cast<uint8_t>(text.size()));
  out->insert(out->end(), text.begin(), text.end());
}

// A varint as the bytes encode_varint produces (the packet ids and every value
// here fit the two-byte case, so this stays a local helper rather than a
// dependency on the codec).
void push_varint(std::vector<uint8_t>* out, uint32_t v) {
  while (v >= 0x80) {
    out->push_back(static_cast<uint8_t>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out->push_back(static_cast<uint8_t>(v));
}

} // namespace

int main() {
  using mc::world::StatKey;

  // ---- award_stats (0x03) -------------------------------------------------
  // Three entries: a mined stone (type 0, block id 1) counted 5, a custom
  // play_time (type 8, custom id 1) counted 20, and a two-byte value id so the
  // varint is not accidentally a byte.
  {
    const std::vector<mc::server::StatEntry> entries = {
        {StatKey{mc::world::kStatMined, 1}, 5},
        {StatKey{mc::world::kStatCustom, mc::world::kCustomPlayTime}, 20},
        {StatKey{mc::world::kStatKilled, 300}, 1},
    };
    const std::vector<uint8_t> want = {
        0x03,                   // award_stats
        0x03,                   // 3 entries
        0x00, 0x01, 0x05,       // mined minecraft:stone x5
        0x08, 0x01, 0x14,       // custom minecraft.play_time x20
        0x06, 0xAC, 0x02, 0x01, // killed id 300 x1
    };
    expect_bytes("award_stats bytes", mc::server::make_award_stats(entries), want);
    // The empty packet is still a packet: a join's first stats request for a
    // player with no counters is a count of zero (`sendStats` sends it).
    const std::vector<uint8_t> want_empty = {0x03, 0x00};
    expect_bytes("award_stats empty", mc::server::make_award_stats({}), want_empty);
  }

  // ---- set_objective (0x6c) ------------------------------------------------
  mc::world::Objective points;
  points.name = "points";
  points.criteria = *mc::world::objective_criteria_by_name("dummy");
  points.display_name = "Points";
  {
    std::vector<uint8_t> want = {0x6C};
    push_varint(&want, 6);
    want.insert(want.end(), {'p', 'o', 'i', 'n', 't', 's'});
    want.push_back(mc::server::kObjectiveMethodAdd);
    push_string_tag(&want, "Points");
    want.push_back(0x00); // render type INTEGER
    want.push_back(0x00); // no number format
    expect_bytes("set_objective add",
                 mc::server::make_set_objective(points, mc::server::kObjectiveMethodAdd), want);
  }
  {
    // HEARTS (render type 1) on a change packet: the same body, method 2.
    mc::world::Objective health = points;
    health.name = "hp";
    health.display_name = "HP";
    health.render_type = mc::world::kRenderHearts;
    std::vector<uint8_t> want = {0x6C};
    push_varint(&want, 2);
    want.insert(want.end(), {'h', 'p'});
    want.push_back(mc::server::kObjectiveMethodChange);
    push_string_tag(&want, "HP");
    want.push_back(0x01); // render type HEARTS
    want.push_back(0x00); // no number format
    expect_bytes("set_objective change (hearts)",
                 mc::server::make_set_objective(health, mc::server::kObjectiveMethodChange), want);
  }
  {
    // Remove stops after the name — which is what the tracking stop needs once
    // the objective is out of the model.
    std::vector<uint8_t> want = {0x6C};
    push_varint(&want, 4);
    want.insert(want.end(), {'g', 'o', 'n', 'e'});
    want.push_back(mc::server::kObjectiveMethodRemove);
    expect_bytes("set_objective remove by name",
                 mc::server::make_set_objective_named("gone", mc::server::kObjectiveMethodRemove),
                 want);
    // ...and the name doubles as the display name for an add.
    std::vector<uint8_t> want_add = {0x6C};
    push_varint(&want_add, 5);
    want_add.insert(want_add.end(), {'f', 'r', 'e', 's', 'h'});
    want_add.push_back(mc::server::kObjectiveMethodAdd);
    push_string_tag(&want_add, "fresh");
    want_add.push_back(0x00);
    want_add.push_back(0x00);
    expect_bytes("set_objective add by name",
                 mc::server::make_set_objective_named("fresh", mc::server::kObjectiveMethodAdd),
                 want_add);
  }

  // ---- set_score (0x70) ----------------------------------------------------
  {
    std::vector<uint8_t> want = {0x70};
    push_varint(&want, 5);
    want.insert(want.end(), {'a', 'l', 'i', 'c', 'e'});
    push_varint(&want, 6);
    want.insert(want.end(), {'p', 'o', 'i', 'n', 't', 's'});
    want.push_back(0x07); // score 7
    want.push_back(0x00); // no display component
    want.push_back(0x00); // no number format
    expect_bytes("set_score bytes", mc::server::make_set_score("alice", "points", 7), want);
  }
  {
    // A negative score is a varint too: -1 is five bytes.
    const std::vector<uint8_t> want = {0x70, 0x05, 'a',  'l',  'i',  'c',  'e',
                                       0x06, 'p',  'o',  'i',  'n',  't',  's',
                                       0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00};
    expect_bytes("set_score negative", mc::server::make_set_score("alice", "points", -1), want);
  }

  // ---- reset_score (0x50) --------------------------------------------------
  {
    const std::vector<uint8_t> none = {0x50, 0x05, 'a', 'l', 'i', 'c', 'e', 0x00};
    expect_bytes("reset_score all objectives", mc::server::make_reset_score("alice", ""), none);
    const std::vector<uint8_t> one = {0x50, 0x05, 'a', 'l', 'i', 'c', 'e', 0x01,
                                      0x06, 'p',  'o', 'i', 'n', 't', 's'};
    expect_bytes("reset_score one objective", mc::server::make_reset_score("alice", "points"), one);
  }

  // ---- set_display_objective (0x64) ---------------------------------------
  {
    const std::vector<uint8_t> sidebar = {0x64, 0x01, 0x06, 'p', 'o', 'i', 'n', 't', 's'};
    expect_bytes("set_display_objective slot 1",
                 mc::server::make_set_display_objective(mc::world::kDisplaySidebar, "points"),
                 sidebar);
    // An empty name is the packet's own "nothing in this slot".
    const std::vector<uint8_t> cleared = {0x64, 0x01, 0x00};
    expect_bytes("set_display_objective cleared",
                 mc::server::make_set_display_objective(mc::world::kDisplaySidebar, ""), cleared);
  }

  // ---- set_player_team (0x6f) ---------------------------------------------
  mc::world::Team red;
  red.name = "red";
  red.display_name = "Red";
  red.prefix = "!";
  red.has_color = true;
  red.color = 12; // TeamColor.RED
  red.players = {"alice", "bob"};
  {
    std::vector<uint8_t> want = {0x6F};
    push_varint(&want, 3);
    want.insert(want.end(), {'r', 'e', 'd'});
    want.push_back(mc::server::kTeamMethodAdd);
    push_string_tag(&want, "Red");
    push_string_tag(&want, "!");
    push_string_tag(&want, "");
    want.push_back(mc::world::kVisibilityAlways);
    want.push_back(mc::world::kCollisionAlways);
    want.push_back(0x01); // colour present
    want.push_back(0x0C); // red
    want.push_back(0x03); // friendly fire + see invisibles
    want.push_back(0x02); // 2 members
    push_varint(&want, 5);
    want.insert(want.end(), {'a', 'l', 'i', 'c', 'e'});
    push_varint(&want, 3);
    want.insert(want.end(), {'b', 'o', 'b'});
    expect_bytes("set_player_team add",
                 mc::server::make_set_player_team(red, mc::server::kTeamMethodAdd), want);
  }
  {
    // Change: the same head, no roster.
    std::vector<uint8_t> want = {0x6F};
    push_varint(&want, 3);
    want.insert(want.end(), {'r', 'e', 'd'});
    want.push_back(mc::server::kTeamMethodChange);
    push_string_tag(&want, "Red");
    push_string_tag(&want, "!");
    push_string_tag(&want, "");
    want.push_back(mc::world::kVisibilityAlways);
    want.push_back(mc::world::kCollisionAlways);
    want.push_back(0x01);
    want.push_back(0x0C);
    want.push_back(0x03);
    expect_bytes("set_player_team change",
                 mc::server::make_set_player_team(red, mc::server::kTeamMethodChange), want);
  }
  {
    // Remove carries the name and the method and nothing else.
    const std::vector<uint8_t> want = {0x6F, 0x03, 'r', 'e', 'd', mc::server::kTeamMethodRemove};
    expect_bytes("set_player_team remove",
                 mc::server::make_set_player_team(red, mc::server::kTeamMethodRemove), want);
  }
  {
    // A joiner's one-name packet (method 3) and the leaver's (method 4); the
    // name is the parameter, not the roster.
    const std::vector<uint8_t> join = {0x6F, 0x03, 'r', 'e', 'd', mc::server::kTeamMethodJoin,
                                       0x01, 0x05, 'a', 'l', 'i', 'c',
                                       'e'};
    expect_bytes("set_player_team join",
                 mc::server::make_set_player_team(red, mc::server::kTeamMethodJoin, "alice"), join);
    const std::vector<uint8_t> leave = {0x6F, 0x03, 'r', 'e', 'd', mc::server::kTeamMethodLeave,
                                        0x01, 0x03, 'b', 'o', 'b'};
    expect_bytes("set_player_team leave",
                 mc::server::make_set_player_team(red, mc::server::kTeamMethodLeave, "bob"), leave);
  }
  {
    // No colour: the optional's own byte, and no value after it.
    mc::world::Team plain;
    plain.name = "t";
    plain.has_color = false;
    plain.allow_friendly_fire = false;
    plain.see_friendly_invisibles = false;
    plain.name_tag_visibility = mc::world::kVisibilityHideForOtherTeams;
    plain.collision_rule = mc::world::kCollisionPushOwnTeam;
    std::vector<uint8_t> want = {0x6F};
    push_varint(&want, 1);
    want.insert(want.end(), {'t'});
    want.push_back(mc::server::kTeamMethodChange);
    push_string_tag(&want, "");
    push_string_tag(&want, "");
    push_string_tag(&want, "");
    want.push_back(mc::world::kVisibilityHideForOtherTeams);
    want.push_back(mc::world::kCollisionPushOwnTeam);
    want.push_back(0x00); // no colour
    want.push_back(0x00); // neither option
    expect_bytes("set_player_team no colour/options",
                 mc::server::make_set_player_team(plain, mc::server::kTeamMethodChange), want);
  }

  if (g_failures == 0) {
    std::printf(
        "PASS: scoreboard-packet\t(award stats + objectives + scores + teams byte layout)\n");
    return 0;
  }
  std::printf("%d scoreboard-packet check(s) failed\n", g_failures);
  return 1;
}
