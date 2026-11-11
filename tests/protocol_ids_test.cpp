// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the generated packet-id tables (protocol/packet_ids.*, the
// Phase-1 "clientbound ids move into data tables" follow-up).
//
// Three things are pinned here, and they cover each other:
//
//   1. **The generated tables are well-formed.** Every table in
//      protocol/packet_ids.generated.h is dense 0x00..0xNN with no duplicate
//      names — the invariant `clientbound_id`/`serverbound_template_id` rely on
//      for a name to resolve unambiguously.
//   2. **Every clientbound id constant means the packet it claims to be.** Each
//      constant in protocol/ids.h is resolved back to its canonical name *and*
//      against the numeric id it had before the tables existed, so a generator
//      bug (a wrong file, a wrong count, a shifted table) fails here rather
//      than at a live client. A row is (state, constant name, value, canonical
//      packet name).
//   3. **The hand-written serverbound dispatch table is 777-correct.** Those
//      rows were transcribed from minecraft-data 26.1 (775) and several carry a
//      "re-verify" note; the generated 777 template is the verification. The
//      crosswalk spells out the two naming schemes side by side (minecraft-data
//      name dispatch keys on -> vanilla's packet-type constant).
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>

#include "protocol/ids.h"
#include "protocol/packet_ids.h"
#include "protocol/packet_table.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) {
    std::printf("  ok   %s\n", what.c_str());
    return;
  }
  std::printf("  FAIL %s\n", what.c_str());
  ++failures;
}

using mc::protocol::clientbound_id;
using mc::protocol::clientbound_packet;
using mc::protocol::kUnknownPacketId;
using mc::protocol::PacketEntry;
using mc::protocol::serverbound_packet;
using mc::protocol::serverbound_template_id;
using mc::protocol::serverbound_template_packet;
using mc::protocol::State;

// The compile-time path the ids.h constants are built on: these evaluate to
// constants, which is the property `checked_packet_id` depends on. If the
// generator ever renames or drops one of these, the build stops here.
static_assert(clientbound_id(State::Play, "login") == 0x32);
static_assert(clientbound_id(State::Play, "add_entity") == 0x01);
static_assert(clientbound_id(State::Status, "status_response") == 0x00);
static_assert(serverbound_template_id(State::Play, "move_player_pos") == 0x1e);

// --- table shape -------------------------------------------------------------

// Dense ids and unique names, per table. `label` names the table in a failure.
template <std::size_t N> void check_table_shape(const char* label, const PacketEntry (&table)[N]) {
  bool dense = true;
  bool unique = true;
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i].id != i)
      dense = false;
    for (std::size_t j = i + 1; j < N; ++j) {
      if (std::string_view(table[i].name) == table[j].name)
        unique = false;
    }
  }
  check(dense, std::string(label) + ": ids are dense 0x00..0x" + std::to_string(N ? N - 1 : 0));
  check(unique, std::string(label) + ": names are unique");
  check(N > 0, std::string(label) + ": is not empty");
}

void test_table_shapes() {
  std::printf("generated tables\n");
  check_table_shape("status/clientbound", mc::protocol::kClientboundStatusPackets);
  check_table_shape("login/clientbound", mc::protocol::kClientboundLoginPackets);
  check_table_shape("configuration/clientbound", mc::protocol::kClientboundConfigurationPackets);
  check_table_shape("play/clientbound", mc::protocol::kClientboundPlayPackets);
  check_table_shape("handshake/serverbound", mc::protocol::kServerboundHandshakePackets);
  check_table_shape("status/serverbound", mc::protocol::kServerboundStatusPackets);
  check_table_shape("login/serverbound", mc::protocol::kServerboundLoginPackets);
  check_table_shape("configuration/serverbound", mc::protocol::kServerboundConfigurationPackets);
  check_table_shape("play/serverbound", mc::protocol::kServerboundPlayPackets);
  check(mc::protocol::kGeneratedProtocolVersion == 777,
        "the tables are the canonical version's (777)");
}

// --- the clientbound constants ----------------------------------------------

struct ClientboundRow {
  State state;
  const char* constant; // the ids.h name, for the failure message
  uint32_t id;          // what the constant was before it was derived
  const char* canonical;
};

constexpr ClientboundRow kClientboundRows[] = {
    // Status (clientbound).
    {State::Status, "kStatusResponseId", 0x00, "status_response"},
    {State::Status, "kStatusPongId", 0x01, "pong_response"},
    // Login (clientbound).
    {State::Login, "kLoginDisconnectId", 0x00, "login_disconnect"},
    {State::Login, "kLoginEncryptionRequestId", 0x01, "hello"},
    {State::Login, "kLoginSuccessId", 0x02, "login_finished"},
    {State::Login, "kLoginSetCompressionId", 0x03, "login_compression"},
    {State::Login, "kLoginPluginRequestId", 0x04, "custom_query"},
    // Configuration (clientbound).
    {State::Configuration, "kConfigPluginMessageId", 0x01, "custom_payload"},
    {State::Configuration, "kConfigFinishId", 0x03, "finish_configuration"},
    {State::Configuration, "kConfigRegistryDataId", 0x07, "registry_data"},
    {State::Configuration, "kConfigFeatureFlagsId", 0x0d, "update_enabled_features"},
    {State::Configuration, "kConfigTagsId", 0x0e, "update_tags"},
    {State::Configuration, "kConfigKnownPacksCbId", 0x0f, "select_known_packs"},
    // Play (clientbound).
    {State::Play, "kPlayBundleDelimiterId", 0x00, "bundle"},
    {State::Play, "kPlaySpawnEntityId", 0x01, "add_entity"},
    {State::Play, "kPlayAnimateId", 0x02, "animate"},
    {State::Play, "kPlayBlockChangedAckId", 0x04, "block_changed_ack"},
    {State::Play, "kPlayBlockDestructionId", 0x05, "block_destruction"},
    {State::Play, "kPlayBlockUpdateId", 0x08, "block_update"},
    {State::Play, "kPlaySetEntityDataId", 0x65, "set_entity_data"},
    {State::Play, "kPlaySetEntityMotionId", 0x67, "set_entity_motion"},
    {State::Play, "kPlayTakeItemEntityId", 0x7f, "take_item_entity"},
    {State::Play, "kPlayContainerCloseId", 0x11, "container_close"},
    {State::Play, "kPlayContainerSetContentId", 0x12, "container_set_content"},
    {State::Play, "kPlayContainerSetDataId", 0x13, "container_set_data"},
    {State::Play, "kPlayContainerSetSlotId", 0x14, "container_set_slot"},
    {State::Play, "kPlayOpenScreenId", 0x3c, "open_screen"},
    {State::Play, "kPlayPlaceGhostRecipeId", 0x40, "place_ghost_recipe"},
    {State::Play, "kPlayRecipeBookAddId", 0x4b, "recipe_book_add"},
    {State::Play, "kPlayRecipeBookSettingsId", 0x4d, "recipe_book_settings"},
    {State::Play, "kPlayUpdateRecipesId", 0x88, "update_recipes"},
    {State::Play, "kPlaySetCursorItemId", 0x62, "set_cursor_item"},
    {State::Play, "kPlaySetExperienceId", 0x69, "set_experience"},
    {State::Play, "kPlayEntityTeleportId", 0x23, "entity_position_sync"},
    {State::Play, "kPlayChunkBatchStartId", 0x0c, "chunk_batch_start"},
    {State::Play, "kPlayChunkBatchFinishedId", 0x0b, "chunk_batch_finished"},
    {State::Play, "kPlayGameEventId", 0x27, "game_event"},
    {State::Play, "kPlayLevelEventId", 0x2f, "level_event"},
    {State::Play, "kPlayJoinGameId", 0x32, "login"},
    {State::Play, "kPlayKeepAliveCbId", 0x2d, "keep_alive"},
    {State::Play, "kPlayChunkDataId", 0x2e, "level_chunk_with_light"},
    {State::Play, "kPlayLightUpdateId", 0x31, "light_update"},
    {State::Play, "kPlayPlayerAbilitiesId", 0x41, "player_abilities"},
    {State::Play, "kPlayPlayerChatId", 0x42, "player_chat"},
    {State::Play, "kPlayPlayerInfoRemoveId", 0x46, "player_info_remove"},
    {State::Play, "kPlayPlayerInfoUpdateId", 0x47, "player_info_update"},
    {State::Play, "kPlayRemoveEntitiesId", 0x4e, "remove_entities"},
    {State::Play, "kPlayPlayerPositionId", 0x49, "player_position"},
    {State::Play, "kPlaySystemChatId", 0x7c, "system_chat"},
    {State::Play, "kPlaySetDefaultSpawnId", 0x63, "set_default_spawn_position"},
    {State::Play, "kPlaySetCenterChunkId", 0x60, "set_chunk_cache_center"},
    {State::Play, "kPlaySetChunkCacheRadiusId", 0x61, "set_chunk_cache_radius"},
    {State::Play, "kPlayRelEntityMoveId", 0x36, "move_entity_pos"},
    {State::Play, "kPlayEntityMoveLookId", 0x37, "move_entity_pos_rot"},
    {State::Play, "kPlayEntityLookId", 0x39, "move_entity_rot"},
    {State::Play, "kPlaySetHeldSlotId", 0x6b, "set_held_slot"},
    {State::Play, "kPlaySetTimeId", 0x73, "set_time"},
    {State::Play, "kPlayUpdateTagsId", 0x89, "update_tags"},
    {State::Play, "kPlayDisconnectId", 0x20, "disconnect"},
    {State::Play, "kPlayPingCbId", 0x3e, "ping"},
    {State::Play, "kPlayPongCbId", 0x3f, "pong_response"},
    {State::Play, "kPlayChangeDifficultyId", 0x0a, "change_difficulty"},
    {State::Play, "kPlayDamageEventId", 0x19, "damage_event"},
    {State::Play, "kPlayEntityEventId", 0x22, "entity_event"},
    {State::Play, "kPlayHurtAnimationId", 0x2b, "hurt_animation"},
    {State::Play, "kPlayPlayerCombatKillId", 0x45, "player_combat_kill"},
    {State::Play, "kPlayRespawnId", 0x54, "respawn"},
    {State::Play, "kPlaySetEquipmentId", 0x68, "set_equipment"},
    {State::Play, "kPlaySetHealthId", 0x6a, "set_health"},
    {State::Play, "kPlayUpdateAttributesId", 0x86, "update_attributes"},
    {State::Play, "kPlayRemoveMobEffectId", 0x4f, "remove_mob_effect"},
    {State::Play, "kPlayUpdateMobEffectId", 0x87, "update_mob_effect"},
    {State::Play, "kPlayCooldownId", 0x16, "cooldown"},
};

void test_clientbound_constants() {
  std::printf("clientbound constants (%zu rows)\n", std::size(kClientboundRows));
  int resolved = 0;
  for (const ClientboundRow& row : kClientboundRows) {
    const std::string what = std::string(row.constant) + " is " + row.canonical;
    const uint32_t by_name = clientbound_id(row.state, row.canonical);
    const PacketEntry* by_id = clientbound_packet(row.state, row.id);
    if (by_name == row.id && by_id != nullptr && std::string_view(by_id->name) == row.canonical) {
      ++resolved;
      continue;
    }
    check(false, what + " (table says id 0x" + std::to_string(by_name) + ", constant was 0x" +
                     std::to_string(row.id) + ")");
  }
  check(resolved == static_cast<int>(std::size(kClientboundRows)),
        "every clientbound constant resolves to the name and id it pins");
}

void test_lookup_round_trips() {
  std::printf("lookup round-trips\n");
  // Unknown names and ids are answered, not approximated.
  check(clientbound_id(State::Play, "no_such_packet") == kUnknownPacketId,
        "an unknown clientbound name is kUnknownPacketId");
  check(clientbound_packet(State::Play, 0xffff) == nullptr, "an unknown clientbound id is nullptr");
  check(clientbound_packet(State::Configuration, 0x14) != nullptr,
        "the last configuration clientbound packet is in the table");
  check(clientbound_packet(State::Configuration, 0x15) == nullptr,
        "and the id just past it is not");
  check(clientbound_id(State::Handshake, "status_response") == kUnknownPacketId,
        "the handshake has no clientbound packets at all");
  // The two tables agree on a packet both carry.
  check(clientbound_id(State::Play, "keep_alive") == 0x2d &&
            clientbound_id(State::Configuration, "keep_alive") == 0x04,
        "a shared packet name keeps its own id per state");
  check(serverbound_template_id(State::Play, "no_such_packet") == kUnknownPacketId,
        "an unknown serverbound name is kUnknownPacketId");
  check(serverbound_template_packet(State::Play, 0x44) != nullptr &&
            serverbound_template_packet(State::Play, 0x45) == nullptr,
        "the generated serverbound play table ends at 0x44");
}

// --- the dispatch table against the generated template -----------------------

struct ServerboundRow {
  uint32_t id;
  const char* dispatch_name; // minecraft-data name the session keys off
  const char* vanilla_name;  // generated canonical name; null = not a 777 packet
};

// Login: the minecraft-data names predate the 1.19+ "hello/key" vocabulary.
constexpr ServerboundRow kLoginRows[] = {
    {0x00, "login_start", "hello"},
    {0x01, "encryption_begin", "key"},
    {0x02, "login_plugin_response", "custom_query_answer"},
    {0x03, "login_acknowledged", "login_acknowledged"},
    {0x04, "cookie_response", "cookie_response"},
};

constexpr ServerboundRow kStatusRows[] = {
    {0x00, "ping_start", "status_request"},
    {0x01, "ping", "ping_request"},
};

constexpr ServerboundRow kConfigurationRows[] = {
    {0x00, "settings", "client_information"},
    {0x01, "cookie_response", "cookie_response"},
    {0x02, "custom_payload", "custom_payload"},
    {0x03, "finish_configuration", "finish_configuration"},
    {0x04, "keep_alive", "keep_alive"},
    {0x05, "pong", "pong"},
    {0x06, "resource_pack_receive", "resource_pack"},
    {0x07, "select_known_packs", "select_known_packs"},
    {0x08, "custom_click_action", "custom_click_action"},
    {0x09, "accept_code_of_conduct", "accept_code_of_conduct"},
};

constexpr ServerboundRow kHandshakeRows[] = {
    {0x00, "set_protocol", "intention"},
    // The legacy pre-1.7 ping has no `addPacket` registration: 0xFE is handled
    // as a special case before the table, so it has no generated counterpart.
    {0xFE, "legacy_server_list_ping", nullptr},
};

// Every id the dispatch table recognizes, id for id. This is the check the
// "re-verify on first real-client observation" notes on these rows were
// waiting for: the ids and the packet behind each one now come from vanilla's
// own registration order, not from an older protocol snapshot.
constexpr ServerboundRow kPlayRows[] = {
    {0x00, "teleport_confirm", "accept_teleportation"},
    {0x01, "attack", "attack"},
    {0x02, "query_block_nbt", "block_entity_tag_query"},
    {0x03, "select_bundle_item", "bundle_item_selected"},
    {0x04, "set_difficulty", "change_difficulty"},
    {0x05, "change_gamemode", "change_game_mode"},
    {0x06, "message_acknowledgement", "chat_ack"},
    {0x07, "chat_command", "chat_command"},
    {0x08, "chat_command_signed", "chat_command_signed"},
    {0x09, "chat_message", "chat"},
    {0x0a, "chat_session_update", "chat_session_update"},
    {0x0b, "chunk_batch_received", "chunk_batch_received"},
    {0x0c, "client_command", "client_command"},
    {0x0d, "tick_end", "client_tick_end"},
    {0x0e, "settings", "client_information"},
    {0x0f, "tab_complete", "command_suggestion"},
    {0x10, "configuration_acknowledged", "configuration_acknowledged"},
    {0x11, "container_button_click", "container_button_click"},
    {0x12, "window_click", "container_click"},
    {0x13, "close_window", "container_close"},
    {0x14, "set_slot_state", "container_slot_state_changed"},
    {0x15, "cookie_response", "cookie_response"},
    {0x16, "custom_payload", "custom_payload"},
    {0x17, "debug_subscription_request", "debug_subscription_request"},
    {0x18, "edit_book", "edit_book"},
    {0x19, "query_entity_nbt", "entity_tag_query"},
    {0x1a, "use_entity", "interact"},
    {0x1b, "generate_structure", "jigsaw_generate"},
    {0x1c, "keep_alive", "keep_alive"},
    {0x1d, "lock_difficulty", "lock_difficulty"},
    {0x1e, "position", "move_player_pos"},
    {0x1f, "position_look", "move_player_pos_rot"},
    {0x20, "look", "move_player_rot"},
    {0x21, "flying", "move_player_status_only"},
    {0x22, "vehicle_move", "move_vehicle"},
    {0x23, "steer_boat", "paddle_boat"},
    {0x24, "pick_item_from_block", "pick_item_from_block"},
    {0x25, "pick_item_from_entity", "pick_item_from_entity"},
    {0x26, "ping_request", "ping_request"},
    {0x27, "craft_recipe_request", "place_recipe"},
    {0x28, "abilities", "player_abilities"},
    {0x29, "block_dig", "player_action"},
    {0x2a, "player_command", "player_command"},
    {0x2b, "player_input", "player_input"},
    {0x2c, "player_loaded", "player_loaded"},
    {0x2d, "pong", "pong"},
    {0x2e, "punch", "punch"},
    {0x2f, "recipe_book", "recipe_book_change_settings"},
    {0x30, "displayed_recipe", "recipe_book_seen_recipe"},
    {0x31, "name_item", "rename_item"},
    {0x32, "resource_pack_receive", "resource_pack"},
    {0x33, "advancement_tab", "seen_advancements"},
    {0x34, "select_trade", "select_trade"},
    {0x35, "set_beacon_effect", "set_beacon"},
    {0x36, "held_item_slot", "set_carried_item"},
    {0x37, "update_command_block", "set_command_block"},
    {0x38, "update_command_block_minecart", "set_command_minecart"},
    {0x39, "set_creative_slot", "set_creative_mode_slot"},
    {0x3a, "set_game_rule", "set_game_rule"},
    {0x3b, "update_jigsaw_block", "set_jigsaw_block"},
    {0x3c, "update_structure_block", "set_structure_block"},
    {0x3d, "set_test_block", "set_test_block"},
    {0x3e, "update_sign", "sign_update"},
    {0x3f, "spectate_entity", "spectator_action"},
    {0x40, "spectate", "teleport_to_entity"},
    {0x41, "test_instance_block_action", "test_instance_block_action"},
    {0x42, "block_place", "use_item_on"},
    {0x43, "use_item", "use_item"},
    {0x44, "custom_click_action", "custom_click_action"},
};

template <std::size_t N>
void check_serverbound_rows(State state, const char* label, const ServerboundRow (&rows)[N]) {
  int matched = 0;
  for (const ServerboundRow& row : rows) {
    const PacketEntry* dispatch = serverbound_packet(state, row.id);
    if (dispatch == nullptr || std::string_view(dispatch->name) != row.dispatch_name) {
      check(false, std::string(label) + " 0x" + std::to_string(row.id) + " is " +
                       row.dispatch_name + " (dispatch table says " +
                       (dispatch ? std::string(dispatch->name) : std::string("<none>")) + ")");
      continue;
    }
    if (row.vanilla_name == nullptr) {
      ++matched;
      continue;
    }
    const PacketEntry* generated = serverbound_template_packet(state, row.id);
    if (generated == nullptr || std::string_view(generated->name) != row.vanilla_name) {
      check(false, std::string(label) + " 0x" + std::to_string(row.id) + " is 26.3 " +
                       row.vanilla_name + " (generated table says " +
                       (generated ? std::string(generated->name) : std::string("<none>")) + ")");
      continue;
    }
    ++matched;
  }
  check(matched == static_cast<int>(N), std::string(label) + ": all " + std::to_string(N) +
                                            " dispatch rows match the generated 777 template");

  // And the other direction: no row the crosswalk forgot. Walk the id space the
  // dispatch table answers in and count what it holds.
  int rows_in_table = 0;
  for (uint32_t id = 0; id < 0x100; ++id) {
    if (serverbound_packet(state, id) != nullptr)
      ++rows_in_table;
  }
  check(rows_in_table == static_cast<int>(N),
        std::string(label) + ": the dispatch table holds no row the crosswalk misses");
}

void test_serverbound_dispatch() {
  std::printf("serverbound dispatch vs the generated 777 template\n");
  check_serverbound_rows(State::Handshake, "handshake", kHandshakeRows);
  check_serverbound_rows(State::Status, "status", kStatusRows);
  check_serverbound_rows(State::Login, "login", kLoginRows);
  check_serverbound_rows(State::Configuration, "configuration", kConfigurationRows);
  check_serverbound_rows(State::Play, "play", kPlayRows);
  // The play table is the one that must be complete: the template has 69
  // packets and the dispatch table recognizes every one of them.
  check(std::size(kPlayRows) == 69, "the play dispatch table covers all 69 serverbound packets");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("protocol_ids_test\n");
  test_table_shapes();
  test_clientbound_constants();
  test_lookup_round_trips();
  test_serverbound_dispatch();
  if (failures != 0) {
    std::printf("protocol_ids_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("protocol_ids_test: all checks passed\n");
  return 0;
}
