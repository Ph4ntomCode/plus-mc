// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cstring>

#include "core/json_util.h"
#include "core/log.h"
#include "net/byte_buffer.h"
#include "protocol/framing.h"
#include "protocol/ids.h"
#include "protocol/packet_table.h"
#include "translation/tier.h"

namespace mc::server {

namespace {

using mc::core::json_escape;
using mc::protocol::State; // handshake next_state values

// Status clientbound ids.
constexpr int kStatusResponseId = 0x00;
constexpr int kStatusPongId = 0x01;

// JSON-string escaping: quotes + backslashes, and control characters as \u00XX
// (raw control bytes would make the response invalid JSON). Iterate as unsigned
// char: UTF-8 continuation bytes are negative as plain char and must pass
// through untouched, never be treated as control characters.

std::string status_json(const StatusInfo& info) {
  return "{\"version\":{\"name\":\"" + json_escape(info.version_name) +
         "\",\"protocol\":" + std::to_string(info.protocol) +
         "},\"players\":{\"max\":" + std::to_string(info.max_players) +
         ",\"online\":" + std::to_string(info.online_players) +
         ",\"sample\":[]},\"description\":{\"text\":\"" + json_escape(info.motd) +
         "\"},\"enforcesSecureChat\":false,\"previewsChat\":false}";
}

bool read_handshake(mc::net::Stream& stream, State* next_state, uint32_t* client_protocol) {
  auto frame = mc::protocol::read_frame(stream, /*threshold=*/-1);
  if (!frame)
    return false;

  mc::net::Reader r(*frame);
  uint32_t packet_id = 0;
  if (!r.varint(&packet_id))
    return false;
  const auto* pkt = mc::protocol::serverbound_packet(State::Handshake, packet_id);
  if (!pkt || std::strcmp(pkt->name, "set_protocol") != 0) {
    log::debug("status: expected handshake (id ", packet_id, ")");
    return false;
  }
  uint32_t protocol = 0;
  std::string address;
  uint16_t port = 0;
  uint32_t next = 0;
  if (!r.varint(&protocol) || !r.string(&address) || !r.u16(&port) || !r.varint(&next)) {
    log::debug("status: malformed handshake");
    return false;
  }
  *next_state = static_cast<State>(next);
  *client_protocol = protocol;
  log::debug("status: handshake protocol=", protocol, " address=", address, " port=", port,
             " next=", next);
  return true;
}

} // namespace

void run_status_exchange(mc::net::Stream& stream, const StatusInfo& info) {
  while (true) {
    auto frame = mc::protocol::read_frame(stream, /*threshold=*/-1);
    if (!frame)
      return;

    mc::net::Reader r(*frame);
    uint32_t packet_id = 0;
    if (!r.varint(&packet_id))
      return;
    const auto* pkt = mc::protocol::serverbound_packet(State::Status, packet_id);
    if (pkt && std::strcmp(pkt->name, "ping_start") == 0) {
      // Status response: [0x00][JSON string].
      mc::net::Writer payload;
      payload.varint(kStatusResponseId);
      payload.string(status_json(info));
      if (!mc::protocol::write_frame(stream, -1, std::move(payload).take()))
        return; // peer gone; don't wait on a dead socket
    } else if (pkt && std::strcmp(pkt->name, "ping") == 0) {
      // Pong echoes the 8-byte ping payload.
      int64_t ping = 0;
      if (!r.i64(&ping))
        return;
      mc::net::Writer payload;
      payload.varint(kStatusPongId);
      payload.i64(ping);
      if (!mc::protocol::write_frame(stream, -1, std::move(payload).take()))
        return;
      return; // session done: a vanilla client disconnects after the pong
    } else {
      log::debug("status: unexpected packet ", pkt ? pkt->name : "(unknown id)");
      return;
    }
  }
}

void run_status_session(mc::net::Stream& stream, const StatusInfo& info) {
  State state = State::Handshake;
  uint32_t client_protocol = 0;
  if (!read_handshake(stream, &state, &client_protocol))
    return;
  if (state != State::Status) {
    log::debug("status: session asked for state ", static_cast<int>(state), " — closing");
    return;
  }
  StatusInfo negotiated = info;
  if (const auto* version = mc::translation::t1_status_version(static_cast<int>(client_protocol))) {
    negotiated.protocol = version->protocol;
    negotiated.version_name.assign(version->name);
  }
  run_status_exchange(stream, negotiated);
}

} // namespace mc::server
