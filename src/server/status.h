// SPDX-License-Identifier: GPL-3.0-or-later
//
// Server List Ping slice (Phase 1): handles one connection through the
// handshake (next_state = status) → status request → ping/pong exchange.
// Packet ids and flow are identical across every supported version era, so
// this is also the conformance baseline for the T0 codec architecture.  Phase 9
// adds one version-dependent field here: a T1 status response advertises the
// client's own protocol instead of canonical 777.
#pragma once

#include <cstdint>
#include <string>

#include "net/stream.h"

namespace mc::server {

struct StatusInfo {
  std::string version_name; // e.g. "26.3"
  int protocol = 0;         // e.g. 777
  std::string motd;         // plain-text description
  int max_players = 20;
  int online_players = 0;
};

// Runs the status flow to completion on `stream` (returns on disconnect/error).
// Reads the handshake itself; use run_status_exchange when the handshake was
// already consumed by a session dispatcher.
void run_status_session(mc::net::Stream& stream, const StatusInfo& info);

// Status request/ping exchange after the handshake has been read.
void run_status_exchange(mc::net::Stream& stream, const StatusInfo& info);

} // namespace mc::server
