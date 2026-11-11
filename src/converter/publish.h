// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase 7 slice 9: turning the Verified catalog into something a static host
// can serve and a reviewer can check.
//
// Three pieces, each with its own artifact:
//
//   * `history.tsv` — an append-only ledger, one row per jar per run, so a
//     score moving between runs is a line in a file that reviews like a diff
//     rather than an opaque blob. A re-scan of an unchanged tree appends
//     nothing, so publishing twice is byte-identical.
//   * `--publish <dir>` — the deployable directory: `index.json`,
//     `verified.md`, `history.tsv`, `index.html`, plus `SHA256SUMS` over all
//     four.
//   * `--sign-key` / `verify-site` — an Ed25519 signature over the exact
//     `SHA256SUMS` bytes and the public key that made it, so a reviewer can
//     prove the page they are reading is the page CI produced. Nothing here
//     loads, links or executes a plugin.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "converter/catalog.h"

namespace mc::converter {

// How many runs the ledger keeps before the oldest rows are dropped. A run is
// one scan; a site re-published unchanged does not add one.
inline constexpr unsigned kMaxHistoryRuns = 200;

// Appends one run of `catalog` to the ledger at `path`, creating it when it
// does not exist. The delta is computed against the previous run: files the
// scan added, files it no longer has, and files whose status, score, coverage,
// name or version moved. When the latest run already describes exactly this
// scan nothing is written and `appended` is false.
HistoryDelta append_history(const std::filesystem::path& path, const Catalog& catalog);

// Renders a delta as the lines `catalog` prints (and `--publish` keeps in the
// site's own record): a count line plus one line per change.
std::string history_text(const HistoryDelta& delta);

struct SiteFile {
  std::string name;
  std::string sha256;
  std::size_t size = 0;
};

struct SiteReport {
  bool ok = false;
  std::string error;
  std::string directory;
  bool signed_site = false;
  std::vector<SiteFile> files;
};

// Writes the deployable site into `directory`. `catalog.history` must be set:
// publishing always records a run, so a site cannot ship without a ledger.
// `signing_key` is an Ed25519 private key in PEM form; empty publishes an
// unsigned site (the sums still prove integrity, not authorship).
SiteReport publish_site(const std::filesystem::path& directory, const Catalog& catalog,
                        std::size_t top, const std::filesystem::path& signing_key);

struct VerifyReport {
  bool ok = false;
  std::string error;
  std::string directory;
  std::size_t checked = 0;
  bool signature_checked = false;
  std::vector<std::string> notes;
};

// Checks every file `SHA256SUMS` lists and, when `key` is non-empty, verifies
// `verified.sig` over the exact `SHA256SUMS` bytes. A published site always
// carries `index.json`, `verified.md`, `history.tsv`, `index.html` and
// `SHA256SUMS`; a sums file that omits one of them is refused because the
// missing file would otherwise be silently unchecked.
VerifyReport verify_site(const std::filesystem::path& directory, const std::filesystem::path& key);

std::string site_report_text(const SiteReport& report);
std::string site_report_json(const SiteReport& report);
std::string verify_report_text(const VerifyReport& report);
std::string verify_report_json(const VerifyReport& report);

} // namespace mc::converter
