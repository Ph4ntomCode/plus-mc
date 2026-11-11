// SPDX-License-Identifier: GPL-3.0-or-later
//
// Data-driven packet id tables for the canonical version (T0 codec
// architecture, docs/PLAN.md §6.1). A numeric packet id means nothing on its
// own: it is one protocol version's registration order, and it moves between
// versions. The *canonical name* is the stable contract, so both directions
// resolve through a table — serverbound dispatch keys off names it gets from
// protocol/packet_table.*, and the clientbound ids the send paths use are
// derived from the tables here (protocol/ids.h) rather than transcribed by
// hand.
//
// The tables themselves are generated from vanilla 26.3's official
// reports/packets.json by tools/gen_protocol_table.py; this header owns only the types and
// the lookups, so regenerating for a new release cannot introduce a mismatch
// that is only caught at runtime. A name that no longer exists is a *compile*
// error in the constant that names it (see `checked_packet_id`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mc::protocol {

// Connection states (wire semantics).
enum class State : int {
  Handshake = 0,
  Status = 1,
  Login = 2,
  Configuration = 3,
  Play = 4,
};

// One packet: its wire id in the table's protocol version and the canonical
// name it is keyed by. Names follow vanilla's own packet-type constants
// (`GamePacketTypes.CLIENTBOUND_ADD_ENTITY` -> "add_entity"); the serverbound
// dispatch table carries minecraft-data's older spellings, which the
// cross-check in tests/protocol_ids_test.cpp maps explicitly.
struct PacketEntry {
  uint32_t id;
  const char* name;
};

} // namespace mc::protocol

// Generated tables live in their own header (clang-format off, do not edit).
// `PacketEntry` has to be declared first, hence the mid-file include.
#include "protocol/packet_ids.generated.h"

namespace mc::protocol {

// What a name lookup returns when the name is not in the table. Deliberately
// not a valid id, so `checked_packet_id` can turn it into a compile error.
constexpr uint32_t kUnknownPacketId = 0xFFFFFFFFu;

template <std::size_t N>
constexpr uint32_t id_by_name(const PacketEntry (&table)[N], std::string_view name) {
  for (std::size_t i = 0; i < N; ++i) {
    if (std::string_view(table[i].name) == name)
      return table[i].id;
  }
  return kUnknownPacketId;
}

// The tables are dense and ascending, so the id lookup is a binary search.
template <std::size_t N>
constexpr const PacketEntry* entry_by_id(const PacketEntry (&table)[N], uint32_t id) {
  std::size_t lo = 0, hi = N;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (table[mid].id == id)
      return &table[mid];
    if (table[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  return nullptr;
}

// Turns a compile-time lookup into a compile error when the name is absent: a
// `constexpr` id constant initialized through this fails to build (the throw is
// not a constant expression) instead of silently becoming a sentinel that sends
// the wrong packet id. Runtime callers never see the throw.
constexpr int checked_packet_id(uint32_t id) {
  if (id == kUnknownPacketId)
    throw "unknown packet name in the protocol table";
  return static_cast<int>(id);
}

// clientbound: id by canonical name, entry by id.
constexpr uint32_t clientbound_id(State state, std::string_view name) {
  switch (state) {
  case State::Status:
    return id_by_name(kClientboundStatusPackets, name);
  case State::Login:
    return id_by_name(kClientboundLoginPackets, name);
  case State::Configuration:
    return id_by_name(kClientboundConfigurationPackets, name);
  case State::Play:
    return id_by_name(kClientboundPlayPackets, name);
  case State::Handshake:
    break; // the handshake has no clientbound packets
  }
  return kUnknownPacketId;
}

constexpr const PacketEntry* clientbound_packet(State state, uint32_t id) {
  switch (state) {
  case State::Status:
    return entry_by_id(kClientboundStatusPackets, id);
  case State::Login:
    return entry_by_id(kClientboundLoginPackets, id);
  case State::Configuration:
    return entry_by_id(kClientboundConfigurationPackets, id);
  case State::Play:
    return entry_by_id(kClientboundPlayPackets, id);
  case State::Handshake:
    break;
  }
  return nullptr;
}

// The generated serverbound template. This is *not* the dispatch table
// (protocol/packet_table.cpp carries only the packets the server recognizes,
// under minecraft-data names); it is the ground truth those rows are verified
// against, one id at a time.
constexpr uint32_t serverbound_template_id(State state, std::string_view name) {
  switch (state) {
  case State::Handshake:
    return id_by_name(kServerboundHandshakePackets, name);
  case State::Status:
    return id_by_name(kServerboundStatusPackets, name);
  case State::Login:
    return id_by_name(kServerboundLoginPackets, name);
  case State::Configuration:
    return id_by_name(kServerboundConfigurationPackets, name);
  case State::Play:
    return id_by_name(kServerboundPlayPackets, name);
  }
  return kUnknownPacketId;
}

constexpr const PacketEntry* serverbound_template_packet(State state, uint32_t id) {
  switch (state) {
  case State::Handshake:
    return entry_by_id(kServerboundHandshakePackets, id);
  case State::Status:
    return entry_by_id(kServerboundStatusPackets, id);
  case State::Login:
    return entry_by_id(kServerboundLoginPackets, id);
  case State::Configuration:
    return entry_by_id(kServerboundConfigurationPackets, id);
  case State::Play:
    return entry_by_id(kServerboundPlayPackets, id);
  }
  return nullptr;
}

} // namespace mc::protocol
