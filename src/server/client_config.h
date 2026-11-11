// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-version client-configuration assets: the synchronized-registry entry
// listings and the Update Tags payload that the Configuration phase must send a
// vanilla 26.x client before it enters Play.
//
// These assets live under data/vanilla/<version>/clientconfig/ (regenerated per
// Mojang release):
//   registries/<suffix>.txt   one entry name per line, in wire order, for each
//                             synchronized registry id "minecraft:<suffix>"
//   tags.bin                  the Update Tags packet payload, byte-exact
//
// Interim sourcing: captured from a real vanilla server join on the same
// version + known-packs (minecraft:core/<version>), which is what lets the
// registry packets carry entry listings with no NBT (the client fills data from
// its own builtin packs). Phase 2 replaces this with a generator that builds
// both from the extracted vanilla data (registries from data/minecraft/<suffix>/
// folders; tags from a hardcoded-registry id dump) — see docs/PHASE1.md.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mc::server {

struct ClientConfigData {
  // The Minecraft version these assets describe (e.g. "26.2"); empty if none.
  std::string version;
  // Synchronized registries in wire order: (registry id, ordered entry names).
  std::vector<std::pair<std::string, std::vector<std::string>>> registries;
  // Raw Update Tags payload (a single packet body).
  std::vector<uint8_t> tags;

  bool ok() const {
    return !version.empty();
  }
};

// Loads the clientconfig assets for the newest extracted version under
// `data_root` (same discovery rule as the version advertisement). Returns an
// asset set with ok() == false when none is present.  When `version` is set,
// that explicit directory is used instead; T1 needs 1.21.11 assets while the
// canonical session keeps the newest (26.3) set loaded.
ClientConfigData load_client_config(const std::string& data_root, const std::string& version = {});

} // namespace mc::server
