// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-convert catalog`: the fourth converter slice (docs/PLAN.md §9.3). It
// walks a directory of plugin JARs and writes a versioned index — score,
// declared API version, reference coverage and port status — built from the
// same analyzer and mapper objects that produce `analyze --json` and
// `map --json`, so the index cannot drift from the reports it summarizes.
// Nothing here loads, links or executes a jar.

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "converter/jar_scan.h"

namespace mc::converter {

struct CatalogEntry {
  // Relative to the scanned directory, with '/' separators, so the index is
  // the same on every platform.
  std::string file;
  std::string name;
  std::string version;
  std::string api_version;
  Score score = Score::Red;
  // False when the archive could not be analyzed at all; such an entry has no
  // score, because publishing one would be a guess.
  bool has_score = false;
  std::size_t classes = 0;
  std::size_t members = 0;
  std::size_t mapped = 0;
  std::size_t partial = 0;
  std::size_t gap = 0;
  // "portable" (a usable manifest and every reference mapped), "partial" (a
  // scaffold is possible but work remains), "refused" (port would refuse the
  // jar: no manifest or no name) or "unreadable" (the archive did not parse).
  std::string status;
  std::string reason;
};

// One row of the run-over-run ledger (publish.h owns the file). The fields a
// published page shows are carried here so the delta can be printed and
// written as JSON without re-reading the ledger.
struct HistoryEntry {
  std::string file;
  std::string name;
  std::string version;
  std::string status;
  std::string score;
  std::string coverage;
};

struct HistoryChange {
  HistoryEntry entry;
  std::string from_status;
  std::string to_status;
  std::string from_coverage;
  std::string to_coverage;
};

struct HistoryDelta {
  bool ok = false;
  std::string error;
  // The ledger this run belongs to, as an absolute path.
  std::string file;
  unsigned run = 0;
  // False when the latest run already describes exactly this scan (nothing was
  // written, which is what makes publishing twice byte-identical).
  bool appended = false;
  std::vector<HistoryEntry> added;
  std::vector<HistoryEntry> removed;
  std::vector<HistoryChange> changed;
  std::size_t unchanged = 0;
};

struct Catalog {
  std::string directory;
  bool ok = false;
  std::string error;
  std::vector<CatalogEntry> entries;
  std::vector<std::string> notes;
  // Set by the CLI after `append_history`; `catalog_json` carries it so the
  // `--out` index and stdout stay byte-identical.
  std::optional<HistoryDelta> history;
};

Catalog build_catalog(const std::filesystem::path& directory);

std::string catalog_text(const Catalog& catalog);
std::string catalog_json(const Catalog& catalog);

// The same scan as a dependency-free static page (`--publish` writes it as
// `index.html`): no script, no external asset, every cell escaped.
std::string catalog_html(const Catalog& catalog, std::size_t top);

// The Markdown "Verified" report behind `catalog --report`: a deterministic,
// publication-ready ranking of the same scan (portable before partial before
// refused before unreadable, coverage inside a status), capped at `top` rows.
// It is a statement about the API surface, not a portability proof, and says
// so in the document itself.
inline constexpr std::size_t kDefaultCatalogReportTop = 100;
std::string catalog_markdown(const Catalog& catalog, std::size_t top);

} // namespace mc::converter
