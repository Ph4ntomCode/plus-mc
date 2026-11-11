// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 26.1/26.2 wire profile (protocols 775 and 776).  Both releases have the
// same packet ids, generated from their own official reports/packets.json by
// tools/gen_protocol_table.py --prefix T0_775/T0_776.  They differ from 26.3
// where 26.3 inserted new packets, so the canonical ids cannot be reused.
//
// The profile is a translated play path: 26.1/26.2 clients complete
// login/configuration and stream the canonical 26.3 terrain through
// t0_content's block/biome mapping.  Items, entity types and inventory ids are
// mapped, and block interaction, entity tracking, player synched data and
// equipment are implemented for T0.
#pragma once

#include <cstdint>
#include <string_view>

#include "protocol/packet_ids.h"
#include "translation/t0_775_packet_ids.generated.h"
#include "translation/t0_776_packet_ids.generated.h"

namespace mc::translation::t0 {

constexpr bool can_login(int protocol) {
  return protocol == 775 || protocol == 776;
}

// 26.1's Login Success stops after the properties array; 26.2 and 26.3 carry
// the 16-byte session id.  Verified against live 26.1.2 and 26.2 captures.
constexpr bool login_success_has_session_id(int protocol) {
  return protocol == 776;
}

// 26.2 added the authentication flag to Join Game; 26.1 stops after the
// enforces-secure-chat byte.  Verified against live captures (112 bytes for
// 26.1.2, 113 for 26.2 with the same name/spawn settings).
constexpr bool join_has_authentication_flag(int protocol) {
  return protocol == 776;
}

constexpr uint32_t clientbound_id(int protocol, protocol::State state, std::string_view name) {
  if (protocol == 775) {
    switch (state) {
    case protocol::State::Login:
      return protocol::id_by_name(protocol::kT0_775ClientboundLoginPackets, name);
    case protocol::State::Configuration:
      return protocol::id_by_name(protocol::kT0_775ClientboundConfigurationPackets, name);
    case protocol::State::Play:
      return protocol::id_by_name(protocol::kT0_775ClientboundPlayPackets, name);
    case protocol::State::Status:
      return protocol::id_by_name(protocol::kT0_775ClientboundStatusPackets, name);
    case protocol::State::Handshake:
      break;
    }
  } else if (protocol == 776) {
    switch (state) {
    case protocol::State::Login:
      return protocol::id_by_name(protocol::kT0_776ClientboundLoginPackets, name);
    case protocol::State::Configuration:
      return protocol::id_by_name(protocol::kT0_776ClientboundConfigurationPackets, name);
    case protocol::State::Play:
      return protocol::id_by_name(protocol::kT0_776ClientboundPlayPackets, name);
    case protocol::State::Status:
      return protocol::id_by_name(protocol::kT0_776ClientboundStatusPackets, name);
    case protocol::State::Handshake:
      break;
    }
  }
  return protocol::kUnknownPacketId;
}

constexpr const protocol::PacketEntry* serverbound_packet(int protocol, protocol::State state,
                                                          uint32_t id) {
  if (protocol == 775) {
    switch (state) {
    case protocol::State::Handshake:
      return protocol::entry_by_id(protocol::kT0_775ServerboundHandshakePackets, id);
    case protocol::State::Status:
      return protocol::entry_by_id(protocol::kT0_775ServerboundStatusPackets, id);
    case protocol::State::Login:
      return protocol::entry_by_id(protocol::kT0_775ServerboundLoginPackets, id);
    case protocol::State::Configuration:
      return protocol::entry_by_id(protocol::kT0_775ServerboundConfigurationPackets, id);
    case protocol::State::Play:
      return protocol::entry_by_id(protocol::kT0_775ServerboundPlayPackets, id);
    }
  } else if (protocol == 776) {
    switch (state) {
    case protocol::State::Handshake:
      return protocol::entry_by_id(protocol::kT0_776ServerboundHandshakePackets, id);
    case protocol::State::Status:
      return protocol::entry_by_id(protocol::kT0_776ServerboundStatusPackets, id);
    case protocol::State::Login:
      return protocol::entry_by_id(protocol::kT0_776ServerboundLoginPackets, id);
    case protocol::State::Configuration:
      return protocol::entry_by_id(protocol::kT0_776ServerboundConfigurationPackets, id);
    case protocol::State::Play:
      return protocol::entry_by_id(protocol::kT0_776ServerboundPlayPackets, id);
    }
  }
  return nullptr;
}

} // namespace mc::translation::t0
