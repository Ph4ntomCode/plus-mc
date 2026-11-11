// SPDX-License-Identifier: GPL-3.0-or-later
//
// Proxy forwarding (Phase 5, ordered item 7). A server behind BungeeCord or
// Velocity is connected to by the *proxy*, not by the client, so the client's
// identity never reaches the backend by the usual route: the proxy already
// authenticated it (and the link between proxy and backend is not encrypted),
// and has to forward it out of band. Two protocols do that, and this file
// carries both:
//
//   * **Legacy BungeeCord** — the proxy appends the client's data to the
//     handshake's server-address field, NUL-separated (see
//     `parse_legacy_forwarding`). BungeeCord, Velocity's `legacy` mode and
//     every Spigot fork's `bungeecord: true` speak it. It is *unauthenticated*:
//     the address is client-controlled, so only a server whose port is not
//     reachable by clients should trust it (which is why it is opt-in).
//
//   * **Velocity modern** — a `velocity:player_info` Login Plugin Request /
//     Response, whose payload is HMAC-SHA256 signed with a secret shared
//     between the two ends (see `verify_velocity_forwarding`), so a direct
//     client cannot forge an identity even if it can reach the backend.
//
// Both produce the same `ForwardedIdentity`: the player's UUID, name and
// game-profile properties, plus the address the proxy saw the client at. The
// caller (server/session.cpp) substitutes it for whatever Login Start claimed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "server/online_auth.h" // ProfileProperty

namespace mc::server {

// What a proxy forwarded about one player. `ok == false` carries the reason in
// `error` (logged, and for Velocity the text the client is kicked with).
struct ForwardedIdentity {
  bool ok = false;
  std::string error;
  std::string address;                     // the client's real IP, textual
  uint8_t uuid[16]{};                      // the player's UUID, raw bytes
  std::string name;                        // the player's username
  std::vector<ProfileProperty> properties; // textures etc.
};

// --- legacy BungeeCord forwarding ------------------------------------------
//
// The handshake address becomes
//
//   <host>\0<client ip>\0<uuid, undashed>[\0<properties JSON>]
//
// (`net.md_5.bungee.ServerConnector.connected`). The properties array is the
// same shape Mojang's hasJoined returns, e.g.
// `[{"name":"textures","value":"<base64>","signature":"<base64>"}]`. A string
// with no NUL, or one that is not 3-4 NUL-separated fields, is not forwarding
// data and yields `ok == false`.
bool has_legacy_forwarding(const std::string& handshake_address);
ForwardedIdentity parse_legacy_forwarding(const std::string& handshake_address);

// --- Velocity modern forwarding --------------------------------------------
//
// The backend sends a Login Plugin Request on this channel with a one-byte
// payload: the highest forwarding version it understands. The proxy answers
// with `[32-byte HMAC][version varint][address][uuid][username][properties]`,
// the HMAC covering everything after itself.
inline constexpr const char* kVelocityPlayerInfoChannel = "velocity:player_info";
// MODERN_FORWARDING_DEFAULT: address, uuid, username, properties. The versions
// above it add the 1.19 chat-signing key, which this engine does not model, so
// we advertise this one and refuse anything higher rather than misread it.
inline constexpr uint8_t kVelocityForwardingVersion = 1;

// The Login Plugin Request payload: one byte, the version advertised above.
std::vector<uint8_t> velocity_request_payload();

// Verifies the HMAC and decodes the response body. `secret` is the shared
// secret; a bad signature, a short body, a version above the one we advertise,
// or a truncated field is `ok == false`.
ForwardedIdentity verify_velocity_forwarding(const std::string& secret,
                                             const std::vector<uint8_t>& data);

} // namespace mc::server
