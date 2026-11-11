// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first T1 wire profile: Minecraft 1.21.11 (protocol 774).  T1 is a large
// tier and this is deliberately one release, not all nine protocols: a client
// that completes configuration here is served 1.21.11's own packet ids and
// registry payload, then served the 774 play entry (Join Game and void chunks).
//
// The tables are generated from the 1.21.11 server's reports/packets.json by
// tools/gen_protocol_table.py --prefix T1.  Canonical names are the same
// vocabulary the T0 tables use, so the session code can branch on a name rather
// than on a bare number.
#pragma once

#include <cstdint>
#include <string_view>

#include "protocol/packet_ids.h"
#include "translation/t1_packet_ids.generated.h"

namespace mc::translation::t1 {

inline constexpr int kProtocol = 774;
inline constexpr std::string_view kVersion = "1.21.11";

// T1 login/configuration is implemented for 1.21.11 first.  The other T1
// protocols still fail closed in serve_connection; they must not be routed
// through this release's ids.
constexpr bool can_login(int protocol) {
  return protocol == kProtocol;
}

constexpr uint32_t clientbound_id(protocol::State state, std::string_view name) {
  switch (state) {
  case protocol::State::Login:
    return protocol::id_by_name(protocol::kT1ClientboundLoginPackets, name);
  case protocol::State::Configuration:
    return protocol::id_by_name(protocol::kT1ClientboundConfigurationPackets, name);
  case protocol::State::Play:
    return protocol::id_by_name(protocol::kT1ClientboundPlayPackets, name);
  case protocol::State::Status:
    return protocol::id_by_name(protocol::kT1ClientboundStatusPackets, name);
  case protocol::State::Handshake:
    break;
  }
  return protocol::kUnknownPacketId;
}

constexpr const protocol::PacketEntry* serverbound_packet(protocol::State state, uint32_t id) {
  switch (state) {
  case protocol::State::Login:
    return protocol::entry_by_id(protocol::kT1ServerboundLoginPackets, id);
  case protocol::State::Configuration:
    return protocol::entry_by_id(protocol::kT1ServerboundConfigurationPackets, id);
  case protocol::State::Play:
    return protocol::entry_by_id(protocol::kT1ServerboundPlayPackets, id);
  case protocol::State::Status:
    return protocol::entry_by_id(protocol::kT1ServerboundStatusPackets, id);
  case protocol::State::Handshake:
    return protocol::entry_by_id(protocol::kT1ServerboundHandshakePackets, id);
  }
  return nullptr;
}

constexpr int checked_clientbound_id(protocol::State state, std::string_view name) {
  return protocol::checked_packet_id(::mc::translation::t1::clientbound_id(state, name));
}

inline constexpr int kLoginSuccessId =
    checked_clientbound_id(protocol::State::Login, "login_finished");
inline constexpr int kLoginSetCompressionId =
    checked_clientbound_id(protocol::State::Login, "login_compression");
inline constexpr int kConfigDisconnectId =
    checked_clientbound_id(protocol::State::Configuration, "disconnect");
inline constexpr int kConfigPluginMessageId =
    checked_clientbound_id(protocol::State::Configuration, "custom_payload");
inline constexpr int kConfigFinishId =
    checked_clientbound_id(protocol::State::Configuration, "finish_configuration");
inline constexpr int kConfigRegistryDataId =
    checked_clientbound_id(protocol::State::Configuration, "registry_data");
inline constexpr int kConfigFeatureFlagsId =
    checked_clientbound_id(protocol::State::Configuration, "update_enabled_features");
inline constexpr int kConfigTagsId =
    checked_clientbound_id(protocol::State::Configuration, "update_tags");
inline constexpr int kConfigKnownPacksCbId =
    checked_clientbound_id(protocol::State::Configuration, "select_known_packs");
inline constexpr int kPlayDisconnectId =
    checked_clientbound_id(protocol::State::Play, "disconnect");

} // namespace mc::translation::t1
