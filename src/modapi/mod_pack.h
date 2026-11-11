// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deterministic `.pmcmod` creation from a staging directory. The staging
// directory contains `mod.json` with the human-authored fields; this tool
// fills every declared module/asset `sha256` and `size`, writes a canonical
// manifest, signs it when a private key is supplied, and emits a stored ZIP
// with fixed timestamps and sorted entries.

#pragma once

#include <filesystem>
#include <string>

namespace mc::modapi {

struct PackOptions {
  std::filesystem::path source_dir;
  std::filesystem::path output_path;
  // Empty means an unsigned package. A non-empty path is an Ed25519 private
  // key in PEM form; the signature covers the canonical `mod.json` bytes.
  std::filesystem::path signing_key;
};

struct PackReport {
  bool ok = false;
  std::string error;
  std::string archive_sha256;
};

PackReport pack_directory(const PackOptions& options);

} // namespace mc::modapi
