// SPDX-License-Identifier: GPL-3.0-or-later
//
// Protocol tiers for the canonical simulation (26.3 / 777).  This is the
// smallest useful Phase 9 boundary: sessions are classified before any
// translation is attempted, and a tier owns the packet/semantic work that
// follows.  T1 is the first legacy tier (1.20.5-1.21.11, protocols 766-774).
// 1.21.11 (774) can complete login/configuration and enter play with movement
// and chat translation; inventory/entity translation is not wired yet, and
// 766-773 still fail closed.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mc::translation {

enum class Tier {
  unsupported,
  t0, // canonical 26.x family
  t1, // 1.20.5-1.21.11; data-component era
};

struct Version {
  int protocol;
  std::string_view name;
  Tier tier;
};

// Development builds since 1.16.4-pre1 use synthetic protocol numbers at or
// above 0x40000000. They are deliberately absent from the release table: a
// snapshot (including the current 26.4 snapshots) has no released wire contract
// to translate against, so it is refused explicitly rather than treated as an
// unknown release.
inline constexpr bool is_development_protocol(int protocol) {
  return protocol >= 0x40000000;
}

// Newest first.  Keep this table in lockstep with docs/VERSION-MATRIX.md;
// snapshots use synthetic protocol numbers and are deliberately absent.
inline constexpr std::array<Version, 12> kVersions{{
    {777, "26.3", Tier::t0}, // current stable; 26.4 is snapshot-only
    {776, "26.2", Tier::t0},
    {775, "26.1", Tier::t0},
    {774, "1.21.11", Tier::t1},
    {773, "1.21.9", Tier::t1},
    {772, "1.21.7", Tier::t1},
    {771, "1.21.6", Tier::t1},
    {770, "1.21.5", Tier::t1},
    {769, "1.21.4", Tier::t1},
    {768, "1.21.2", Tier::t1},
    {767, "1.21", Tier::t1},
    {766, "1.20.5", Tier::t1},
}};

inline constexpr const Version* version_for(int protocol) {
  for (const Version& version : kVersions)
    if (version.protocol == protocol)
      return &version;
  return nullptr;
}

inline constexpr Tier tier_for(int protocol) {
  const Version* version = version_for(protocol);
  return version != nullptr ? version->tier : Tier::unsupported;
}

// A translated client can complete server-list ping.  This returns the version
// the status response must advertise (its own protocol, not canonical 777), or
// nullptr for canonical/unknown protocols, which keep the configured name.
// 26.1/26.2 are T0 but still need their own advertisement to be compatible.
inline constexpr const Version* status_version(int protocol) {
  const Version* version = version_for(protocol);
  if (version == nullptr || version->protocol == 777)
    return nullptr;
  return version->tier == Tier::t0 || version->tier == Tier::t1 ? version : nullptr;
}

inline constexpr const Version* t1_status_version(int protocol) {
  const Version* version = version_for(protocol);
  return version != nullptr && version->tier == Tier::t1 ? version : nullptr;
}

} // namespace mc::translation
