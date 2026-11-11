// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol/packet_table.h"

#include <cstddef>

namespace mc::protocol {

// Canonical table for protocol 777 (26.3), the Phase-1 target.
//
// Every id listed under "verified" has been observed byte-exactly in a live
// vanilla capture or is exercised by the integration tests; the rest of the
// play rows are checked against the generated 777 registration order in
// tests/protocol_ids_test.cpp.
//
// Names are minecraft-data canonical snake_case (the stable contract behavior
// dispatches on); wiki spellings differ for some (e.g. position == Move Player
// Position, settings == Client Information, ping_start == Status Request).

// clang-format off
constexpr PacketEntry kHandshaking[] = {
  {0x00, "set_protocol"},
  {0xFE, "legacy_server_list_ping"}, // not implemented; recognized for logging
};

// Status: verified in the vanilla capture (ping_start + ping exchange).
constexpr PacketEntry kStatus[] = {
  {0x00, "ping_start"}, // Status Request (wiki)
  {0x01, "ping"},
};

// Login: verified in the vanilla capture / tests (777).
constexpr PacketEntry kLogin[] = {
  {0x00, "login_start"},
  {0x01, "encryption_begin"}, // Encryption Response
  {0x02, "login_plugin_response"},
  {0x03, "login_acknowledged"},
  {0x04, "cookie_response"},
};

// Configuration: verified against the vanilla 777 capture.
constexpr PacketEntry kConfiguration[] = {
  {0x00, "settings"},            // Client Information
  {0x01, "cookie_response"},
  {0x02, "custom_payload"},
  {0x03, "finish_configuration"},
  {0x04, "keep_alive"},
  {0x05, "pong"},
  {0x06, "resource_pack_receive"},
  {0x07, "select_known_packs"},  // Known Packs
  {0x08, "custom_click_action"},
  {0x09, "accept_code_of_conduct"},
};

// Play (serverbound). Names stay minecraft-data's stable dispatch spelling;
// ids are the 26.3 registration order (punch replaces 26.2's swing row).
constexpr PacketEntry kPlay[] = {
  {0x00, "teleport_confirm"},        // Accept Teleportation (verified)
  {0x01, "attack"},                  // 775
  {0x02, "query_block_nbt"},         // 775
  {0x03, "select_bundle_item"},      // 775
  {0x04, "set_difficulty"},          // 775
  {0x05, "change_gamemode"},         // 775
  {0x06, "message_acknowledgement"}, // 775
  {0x07, "chat_command"},            // 775
  {0x08, "chat_command_signed"},     // 775
  {0x09, "chat_message"},            // Chat (verified)
  {0x0a, "chat_session_update"},     // 775
  {0x0b, "chunk_batch_received"},    // verified
  {0x0c, "client_command"},          // 775
  {0x0d, "tick_end"},                // Client Tick End (verified)
  {0x0e, "settings"},                // Client Information (verified)
  {0x0f, "tab_complete"},            // 775
  {0x10, "configuration_acknowledged"}, // 775
  {0x11, "container_button_click"},  // Container Button Click (verified against
                                     // the ordered 26.2 template; 775's
                                     // minecraft-data name here was enchant_item,
                                     // which 26.2 does not have serverbound)
  {0x12, "window_click"},            // 775
  {0x13, "close_window"},            // 775
  {0x14, "set_slot_state"},          // 775
  {0x15, "cookie_response"},         // 775
  {0x16, "custom_payload"},          // 775
  {0x17, "debug_subscription_request"}, // 775
  {0x18, "edit_book"},               // 775
  {0x19, "query_entity_nbt"},        // 775
  {0x1a, "use_entity"},              // 775
  {0x1b, "generate_structure"},      // 775
  {0x1c, "keep_alive"},              // verified
  {0x1d, "lock_difficulty"},         // 775
  {0x1e, "position"},                // Move Player Position (verified)
  {0x1f, "position_look"},           // Move Player Position & Rotation (verified)
  {0x20, "look"},                    // Move Player Rotation (verified)
  {0x21, "flying"},                  // Move Player Status Only (verified)
  {0x22, "vehicle_move"},            // 775
  {0x23, "steer_boat"},              // 775
  {0x24, "pick_item_from_block"},    // 775
  {0x25, "pick_item_from_entity"},   // 775
  {0x26, "ping_request"},            // verified
  {0x27, "craft_recipe_request"},    // Place Recipe (verified: 0x27 is
                                     // SERVERBOUND_PLACE_RECIPE)
  {0x28, "abilities"},               // 775
  {0x29, "block_dig"},               // 775
  {0x2a, "player_command"},          // 776 (was entity_action in 775; the
                                     // action ordinals are STOP_SLEEPING,
                                     // START_SPRINTING, STOP_SPRINTING, ...)
  {0x2b, "player_input"},            // 775
  {0x2c, "player_loaded"},           // verified
  {0x2d, "pong"},                    // 775
  {0x2e, "punch"},                   // ServerboundPunchPacket (replaces 26.2 swing)
  {0x2f, "recipe_book"},             // Change Recipe Book Settings
  {0x30, "displayed_recipe"},        // Set Seen Recipe
  {0x31, "name_item"},               // Rename Item
  {0x32, "resource_pack_receive"},   // Resource Pack Response
  {0x33, "advancement_tab"},         // Seen Advancements
  {0x34, "select_trade"},            // Select Trade
  {0x35, "set_beacon_effect"},       // Set Beacon
  {0x36, "held_item_slot"},          // Set Carried Item
  {0x37, "update_command_block"},    // Set Command Block
  {0x38, "update_command_block_minecart"}, // Set Command Minecart
  {0x39, "set_creative_slot"},       // Set Creative Mode Slot
  {0x3a, "set_game_rule"},           // Set Game Rule
  {0x3b, "update_jigsaw_block"},     // Set Jigsaw Block
  {0x3c, "update_structure_block"},  // Set Structure Block
  {0x3d, "set_test_block"},          // Set Test Block
  {0x3e, "update_sign"},             // Sign Update
  {0x3f, "spectate_entity"},         // Spectator Action
  {0x40, "spectate"},                // Teleport To Entity
  {0x41, "test_instance_block_action"}, // Test Instance Block Action
  {0x42, "block_place"},             // Use Item On
  {0x43, "use_item"},                // Use Item
  {0x44, "custom_click_action"},     // Custom Click Action
};
// clang-format on

namespace {

// Binary search over a sorted table (ids are ascending).
template <size_t N> const PacketEntry* find(const PacketEntry (&table)[N], uint32_t id) {
  size_t lo = 0, hi = N;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (table[mid].id == id)
      return &table[mid];
    if (table[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return nullptr;
}

} // namespace

const PacketEntry* serverbound_packet(State state, uint32_t id) {
  switch (state) {
  case State::Handshake:
    return find(kHandshaking, id);
  case State::Status:
    return find(kStatus, id);
  case State::Login:
    return find(kLogin, id);
  case State::Configuration:
    return find(kConfiguration, id);
  case State::Play:
    return find(kPlay, id);
  }
  return nullptr;
}

} // namespace mc::protocol
