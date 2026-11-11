// SPDX-License-Identifier: GPL-3.0-or-later
//
// Server-side preflight for `.pmcmod` packages (docs/MOD_DISTRIBUTION.md,
// Phase 13A). This is deliberately a verifier, not a loader: it decides
// whether a package may be handed to the existing native plugin host, and
// explains a refusal without executing anything from the package.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::modapi {

struct VerifyOptions {
  // Public keys the operator trusts. Each is a PEM public key, a raw 32-byte
  // Ed25519 key, or 64 hex characters (optionally prefixed `ed25519:`).
  std::vector<std::filesystem::path> trusted_keys;
  // Empty means "this build's host". A non-empty value is for inspecting a
  // package for another machine, not for pretending it is runnable here.
  std::string host_target;
  // Empty means the build's version / ABI.
  std::string engine_version;
  std::string abi_version;
  // Permit a package with no `mod.sig`. A present-but-invalid signature is
  // never excused by this flag.
  bool allow_unsigned = false;
};

struct ModuleReport {
  std::string target;
  std::string path;
  std::string sha256;
  std::string format;
  uint64_t size = 0;
};

struct VerifyReport {
  bool ready = false;
  std::string status;
  std::string reason;
  std::string package;
  std::string archive_sha256;
  std::string id;
  std::string name;
  std::string version;
  std::string host;
  std::string engine;
  std::string api;
  std::string signature;
  ModuleReport module;
  std::vector<std::string> notes;
};

struct ExtractOptions {
  VerifyOptions verify;
  // Replace an existing destination directory only after a complete temp
  // extraction has succeeded. False keeps an operator's directory safe.
  bool replace = false;
};

struct ExtractReport {
  VerifyReport verification;
  bool extracted = false;
  std::string destination;
  std::string error;
};

// `macos-aarch64`, `macos-x86_64`, `linux-x86_64`, `linux-aarch64`,
// `windows-x86_64` or `windows-aarch64`; `"unsupported"` when the build does
// not recognise its own host.
std::string current_host_target();

// Verify a package without extracting it or loading its module. A refusal is a
// normal result with `ready == false`, not an exception.
VerifyReport verify_package(const std::filesystem::path& path, const VerifyOptions& options);

// Verify, then extract into `destination` via a temporary sibling and a rename.
// A refused package is reported in `verification` and never writes anything.
ExtractReport extract_package(const std::filesystem::path& path,
                              const std::filesystem::path& destination,
                              const ExtractOptions& options);

} // namespace mc::modapi
