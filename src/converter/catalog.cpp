// SPDX-License-Identifier: GPL-3.0-or-later
//
// The catalog walks a directory, analyzes each JAR, maps its references and
// records one entry per archive. A jar that cannot be read is a finding in the
// index rather than a reason to abandon the whole scan; only a directory that
// cannot be walked, or one with no jars at all, fails the command.

#include "converter/catalog.h"

#include <algorithm>
#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

#include "converter/api_map.h"
#include "converter/report_json.h"

namespace mc::converter {
namespace {

std::string lowered(std::string value) {
  for (char& c : value) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return value;
}

std::size_t count_status(const MappingReport& report, MapStatus status) {
  std::size_t count = 0;
  for (const ClassMapping& mapping : report.classes) {
    if (mapping.status == status)
      ++count;
  }
  for (const MemberMapping& mapping : report.members) {
    if (mapping.status == status)
      ++count;
  }
  return count;
}

std::size_t reference_total(const CatalogEntry& entry) {
  return entry.mapped + entry.partial + entry.gap;
}

// The same (mapped + partial) / total ratio as `map --json`, so a catalog
// cannot report a different coverage than the report it came from.
double coverage_ratio(const CatalogEntry& entry) {
  const std::size_t total = reference_total(entry);
  return total == 0 ? 0.0 : static_cast<double>(entry.mapped + entry.partial) / total;
}

std::string coverage_text(const CatalogEntry& entry) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", coverage_ratio(entry));
  return text;
}

struct Summary {
  std::size_t jars = 0;
  std::size_t analyzed = 0;
  std::size_t portable = 0;
  std::size_t partial = 0;
  std::size_t refused = 0;
  std::size_t unreadable = 0;
  std::size_t green = 0;
  std::size_t yellow = 0;
  std::size_t red = 0;
};

Summary summarize(const Catalog& catalog) {
  Summary summary;
  for (const CatalogEntry& entry : catalog.entries) {
    ++summary.jars;
    if (entry.status == "portable")
      ++summary.portable;
    else if (entry.status == "partial")
      ++summary.partial;
    else if (entry.status == "refused")
      ++summary.refused;
    else if (entry.status == "unreadable")
      ++summary.unreadable;
    if (!entry.has_score)
      continue;
    ++summary.analyzed;
    if (entry.score == Score::Green)
      ++summary.green;
    else if (entry.score == Score::Yellow)
      ++summary.yellow;
    else
      ++summary.red;
  }
  return summary;
}

// The ranking's status order: a jar a port can scaffold with no API decision
// first, then one that needs work, then the jars `port` would refuse, then the
// archives nobody can read.
int status_rank(const std::string& status) {
  if (status == "portable")
    return 0;
  if (status == "partial")
    return 1;
  if (status == "refused")
    return 2;
  return 3;
}

// Markdown table content: a pipe would split the cell and angle brackets would
// become HTML on a published page. Everything else passes through.
std::string md_text(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '|':
      out += "\\|";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '\n':
    case '\r':
    case '\t':
      out += ' ';
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

std::string md_code(const std::string& value) {
  std::string text = md_text(value);
  for (char& c : text)
    if (c == '`')
      c = '\'';
  return "`" + text + "`";
}

std::string percent_text(const CatalogEntry& entry) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.0f%%", coverage_ratio(entry) * 100.0);
  return text;
}

std::string markdown_why(const CatalogEntry& entry) {
  if (entry.status == "portable")
    return "every reference has a native symbol";
  return entry.reason.empty() ? "not analyzed" : entry.reason;
}

// The review order shared by the Markdown report and the published page:
// portable before partial before refused before unreadable, coverage
// descending inside a status, then the path. A jar with no score sorts last
// within its status because its coverage is zero.
std::vector<const CatalogEntry*> ranked_entries(const Catalog& catalog) {
  std::vector<const CatalogEntry*> ranked;
  ranked.reserve(catalog.entries.size());
  for (const CatalogEntry& entry : catalog.entries)
    ranked.push_back(&entry);
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const CatalogEntry* left, const CatalogEntry* right) {
                     const int left_rank = status_rank(left->status);
                     const int right_rank = status_rank(right->status);
                     if (left_rank != right_rank)
                       return left_rank < right_rank;
                     const double left_coverage = coverage_ratio(*left);
                     const double right_coverage = coverage_ratio(*right);
                     if (left_coverage != right_coverage)
                       return left_coverage > right_coverage;
                     return left->file < right->file;
                   });
  return ranked;
}

std::string html_text(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\n':
    case '\r':
    case '\t':
      out += ' ';
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

} // namespace

Catalog build_catalog(const std::filesystem::path& directory) {
  Catalog catalog;
  std::error_code status_error;
  if (!std::filesystem::is_directory(directory, status_error)) {
    // A missing path is still "not a directory" from the caller's point of
    // view; only a real access failure deserves the operating system's words.
    if (status_error && status_error != std::errc::no_such_file_or_directory)
      catalog.error =
          "cannot inspect " + directory.generic_string() + ": " + status_error.message();
    else
      catalog.error = "not a directory: " + directory.generic_string();
    return catalog;
  }

  std::error_code absolute_error;
  catalog.directory =
      std::filesystem::absolute(directory, absolute_error).lexically_normal().generic_string();
  if (absolute_error)
    catalog.directory = directory.lexically_normal().generic_string();

  std::vector<std::pair<std::string, std::filesystem::path>> jars;
  std::error_code walk_error;
  std::filesystem::recursive_directory_iterator iterator(
      directory, std::filesystem::directory_options::skip_permission_denied, walk_error);
  if (walk_error) {
    catalog.error = "cannot read " + catalog.directory + ": " + walk_error.message();
    return catalog;
  }
  const std::filesystem::recursive_directory_iterator end;
  for (; iterator != end; iterator.increment(walk_error)) {
    if (walk_error)
      break;
    std::error_code type_error;
    if (!iterator->is_regular_file(type_error) || type_error)
      continue;
    if (lowered(iterator->path().extension().string()) != ".jar")
      continue;
    jars.emplace_back(iterator->path().lexically_relative(directory).generic_string(),
                      iterator->path());
  }
  if (walk_error)
    catalog.notes.push_back("the directory walk stopped early: " + walk_error.message());
  if (jars.empty()) {
    catalog.error = "no .jar files under " + catalog.directory;
    return catalog;
  }

  std::sort(jars.begin(), jars.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });

  for (const auto& [relative, path] : jars) {
    CatalogEntry entry;
    entry.file = relative;
    const Analysis analysis = analyze_jar(path);
    if (!analysis.ok) {
      entry.status = "unreadable";
      entry.reason = analysis.error;
      catalog.entries.push_back(std::move(entry));
      continue;
    }

    entry.has_score = true;
    entry.score = analysis.score;
    entry.name = analysis.manifest.name;
    entry.version = analysis.manifest.version;
    entry.api_version = analysis.manifest.api_version;

    const MappingReport mapping = map_analysis(analysis);
    if (!mapping.ok) {
      entry.has_score = false;
      entry.status = "unreadable";
      entry.reason = mapping.error;
      catalog.entries.push_back(std::move(entry));
      continue;
    }

    entry.classes = mapping.classes.size();
    entry.members = mapping.members.size();
    entry.mapped = count_status(mapping, MapStatus::Mapped);
    entry.partial = count_status(mapping, MapStatus::Partial);
    entry.gap = count_status(mapping, MapStatus::Gap);

    if (!analysis.has_manifest || entry.name.empty()) {
      entry.status = "refused";
      entry.reason =
          analysis.has_manifest ? "the manifest has no name" : "no plugin.yml or paper-plugin.yml";
    } else if (entry.partial == 0 && entry.gap == 0) {
      entry.status = "portable";
    } else {
      entry.status = "partial";
      entry.reason = std::to_string(entry.gap) + " gap and " + std::to_string(entry.partial) +
                     " partial reference(s)";
    }
    catalog.entries.push_back(std::move(entry));
  }

  const Summary summary = summarize(catalog);
  if (summary.unreadable != 0) {
    catalog.notes.push_back(std::to_string(summary.unreadable) +
                            " archive(s) could not be read and carry no score");
  }
  if (summary.refused != 0) {
    catalog.notes.push_back(std::to_string(summary.refused) +
                            " jar(s) would be refused by port (no usable manifest)");
  }
  catalog.ok = true;
  return catalog;
}

std::string catalog_text(const Catalog& catalog) {
  std::string out = "+MC converter catalog\n";
  out += "directory: " + catalog.directory + "\n";
  if (!catalog.ok)
    return out + "error: " + catalog.error + "\n";

  const Summary summary = summarize(catalog);
  out += "jars: " + std::to_string(summary.jars) + " (" + std::to_string(summary.portable) +
         " portable, " + std::to_string(summary.partial) + " partial, " +
         std::to_string(summary.refused) + " refused, " + std::to_string(summary.unreadable) +
         " unreadable)\n";
  out += "scores: " + std::to_string(summary.green) + " green, " + std::to_string(summary.yellow) +
         " yellow, " + std::to_string(summary.red) + " red (" + std::to_string(summary.analyzed) +
         " analyzed)\n";

  for (const CatalogEntry& entry : catalog.entries) {
    out += "  " + entry.status + "  " + entry.file;
    if (!entry.name.empty())
      out += "  " + entry.name + " " + entry.version;
    if (entry.status == "portable" || entry.status == "partial") {
      out += "  (api-version " +
             (entry.api_version.empty() ? std::string("none") : entry.api_version) + "; " +
             std::to_string(entry.mapped) + " of " + std::to_string(reference_total(entry)) +
             " mapped";
      if (entry.partial != 0 || entry.gap != 0) {
        out += ", " + std::to_string(entry.partial) + " partial, " + std::to_string(entry.gap) +
               " gap";
      }
      out += ")";
    } else if (!entry.reason.empty()) {
      out += "  (" + entry.reason + ")";
    }
    out += "\n";
  }
  for (const std::string& note : catalog.notes)
    out += "note: " + note + "\n";
  return out;
}

std::string catalog_json(const Catalog& catalog) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"mode\":\"catalog\",\"ok\":";
  out += catalog.ok ? "true" : "false";
  if (!catalog.ok) {
    out += ",\"error\":";
    out += detail::json_quote(catalog.error);
    return out + "}\n";
  }

  out += ",\"abi\":1,\"directory\":";
  out += detail::json_quote(catalog.directory);
  const Summary summary = summarize(catalog);
  out += ",\"summary\":{\"jars\":" + std::to_string(summary.jars);
  out += ",\"analyzed\":" + std::to_string(summary.analyzed);
  out += ",\"portable\":" + std::to_string(summary.portable);
  out += ",\"partial\":" + std::to_string(summary.partial);
  out += ",\"refused\":" + std::to_string(summary.refused);
  out += ",\"unreadable\":" + std::to_string(summary.unreadable);
  out += ",\"green\":" + std::to_string(summary.green);
  out += ",\"yellow\":" + std::to_string(summary.yellow);
  out += ",\"red\":" + std::to_string(summary.red);
  out += "},\"entries\":[";
  for (std::size_t i = 0; i < catalog.entries.size(); ++i) {
    if (i != 0)
      out += ",";
    const CatalogEntry& entry = catalog.entries[i];
    out += "{\"file\":";
    out += detail::json_quote(entry.file);
    out += ",\"name\":";
    out += detail::json_quote(entry.name);
    out += ",\"version\":";
    out += detail::json_quote(entry.version);
    out += ",\"api_version\":";
    out += detail::json_quote(entry.api_version);
    out += ",\"status\":";
    out += detail::json_quote(entry.status);
    out += ",\"score\":";
    out += entry.has_score ? detail::json_quote(score_name(entry.score)) : "null";
    out += ",\"classes\":" + std::to_string(entry.classes);
    out += ",\"members\":" + std::to_string(entry.members);
    out += ",\"mapped\":" + std::to_string(entry.mapped);
    out += ",\"partial_refs\":" + std::to_string(entry.partial);
    out += ",\"gap\":" + std::to_string(entry.gap);
    out += ",\"coverage\":" + coverage_text(entry);
    out += ",\"reason\":";
    out += detail::json_quote(entry.reason);
    out += "}";
  }
  out += "],\"notes\":";
  detail::json_list(&out, catalog.notes);
  if (catalog.history) {
    const HistoryDelta& history = *catalog.history;
    // `appended` is deliberately absent: a re-publish of an unchanged scan
    // reproduces the run's original delta, so the published index stays
    // byte-identical instead of depending on when it was written.
    out += ",\"history\":{\"file\":" + detail::json_quote(history.file) +
           ",\"run\":" + std::to_string(history.run);
    out += ",\"added\":[";
    for (std::size_t i = 0; i < history.added.size(); ++i) {
      if (i != 0)
        out += ",";
      out += "{\"file\":" + detail::json_quote(history.added[i].file) +
             ",\"status\":" + detail::json_quote(history.added[i].status) + "}";
    }
    out += "],\"removed\":[";
    for (std::size_t i = 0; i < history.removed.size(); ++i) {
      if (i != 0)
        out += ",";
      out += "{\"file\":" + detail::json_quote(history.removed[i].file) + "}";
    }
    out += "],\"changed\":[";
    for (std::size_t i = 0; i < history.changed.size(); ++i) {
      if (i != 0)
        out += ",";
      const HistoryChange& change = history.changed[i];
      out += "{\"file\":" + detail::json_quote(change.entry.file) +
             ",\"from\":" + detail::json_quote(change.from_status) +
             ",\"to\":" + detail::json_quote(change.to_status) +
             ",\"from_coverage\":" + detail::json_quote(change.from_coverage) +
             ",\"to_coverage\":" + detail::json_quote(change.to_coverage) + "}";
    }
    out += "],\"unchanged\":" + std::to_string(history.unchanged) + "}";
  }
  out += "}\n";
  return out;
}

std::string catalog_markdown(const Catalog& catalog, std::size_t top) {
  std::string out = "# +MC Verified catalog\n\n";
  out += "API-surface report for `" + catalog.directory + "` from +MC Converter (ABI 1).\n\n";
  if (!catalog.ok) {
    out += "The scan failed: " + md_text(catalog.error) + "\n";
    return out;
  }

  if (top == 0)
    top = 1;
  const Summary summary = summarize(catalog);
  out += "| Field | Value |\n| --- | --- |\n";
  out += "| jars | " + std::to_string(summary.jars) + " (" + std::to_string(summary.portable) +
         " portable, " + std::to_string(summary.partial) + " partial, " +
         std::to_string(summary.refused) + " refused, " + std::to_string(summary.unreadable) +
         " unreadable) |\n";
  out += "| scores | " + std::to_string(summary.green) + " green, " +
         std::to_string(summary.yellow) + " yellow, " + std::to_string(summary.red) + " red (" +
         std::to_string(summary.analyzed) + " analyzed) |\n\n";

  const std::vector<const CatalogEntry*> ranked = ranked_entries(catalog);

  const std::size_t shown = std::min(top, ranked.size());
  out += "## Ranked ports\n\n";
  out += "| # | Jar | Plugin | Status | Coverage | Score | Why |\n";
  out += "| --- | --- | --- | --- | --- | --- | --- |\n";
  for (std::size_t i = 0; i < shown; ++i) {
    const CatalogEntry& entry = *ranked[i];
    out += "| " + std::to_string(i + 1) + " | " + md_code(entry.file) + " | ";
    out +=
        entry.name.empty() ? std::string("-") : md_text(entry.name) + " " + md_text(entry.version);
    out += " | " + md_text(entry.status) + " | ";
    // A refused or unreadable jar has no meaningful reference coverage: the
    // numbers are empty or absent, so the cell says so instead of printing 0%.
    const bool has_coverage = entry.status == "portable" || entry.status == "partial";
    out += has_coverage ? percent_text(entry) : std::string("-");
    out += " | ";
    out += entry.has_score ? std::string(score_name(entry.score)) : std::string("-");
    out += " | " + md_text(markdown_why(entry)) + " |\n";
  }
  out += "\n";
  if (shown < ranked.size()) {
    out += "Showing the first " + std::to_string(shown) + " of " + std::to_string(ranked.size()) +
           " entries; `--top` controls the cap.\n\n";
  }

  out += "## What this is not\n\n";
  out += "- **Not a portability proof.** `portable` means every reference the jar names has a\n";
  out += "  native symbol, not that the plugin has been run against this engine.\n";
  out += "- **Not a finished port.** Generated handlers start from defaults; commands,\n";
  out += "  permissions, scheduling and configuration remain hand work.\n";
  out += "- **Coverage is the mapper's table.** A mapped call can still behave differently.\n\n";
  out += "## Notes\n\n";
  if (catalog.notes.empty()) {
    out += "- none\n";
  } else {
    for (const std::string& note : catalog.notes)
      out += "- " + md_text(note) + "\n";
  }
  return out;
}

std::string catalog_html(const Catalog& catalog, std::size_t top) {
  std::string out = "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n";
  out += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  out += "<title>+MC Verified catalog</title>\n<style>\n";
  out += "body{font:16px/1.5 system-ui,sans-serif;margin:2rem auto;max-width:72rem;padding:0 1rem;"
         "color:#1b1b1b}\n";
  out += "table{border-collapse:collapse;width:100%;margin:1rem 0}\n";
  out += "th,td{border-bottom:1px solid #ddd;padding:.4rem .6rem;text-align:left;"
         "vertical-align:top}\n";
  out += "code{font-size:.9em}\n.portable{color:#0a6b2d}.partial{color:#8a5b00}"
         ".refused{color:#8a1c1c}.unreadable{color:#5a5a5a}\n";
  out += "</style>\n</head>\n<body>\n<h1>+MC Verified catalog</h1>\n";
  out += "<p>API-surface report for <code>" + html_text(catalog.directory) +
         "</code> from +MC Converter (ABI 1).</p>\n";
  if (!catalog.ok) {
    out += "<p>The scan failed: " + html_text(catalog.error) + "</p>\n";
    return out + "</body>\n</html>\n";
  }

  if (top == 0)
    top = 1;
  const Summary summary = summarize(catalog);
  out += "<table>\n<tr><th>jars</th><td>" + std::to_string(summary.jars) + " (" +
         std::to_string(summary.portable) + " portable, " + std::to_string(summary.partial) +
         " partial, " + std::to_string(summary.refused) + " refused, " +
         std::to_string(summary.unreadable) + " unreadable)</td></tr>\n";
  out += "<tr><th>scores</th><td>" + std::to_string(summary.green) + " green, " +
         std::to_string(summary.yellow) + " yellow, " + std::to_string(summary.red) + " red (" +
         std::to_string(summary.analyzed) + " analyzed)</td></tr>\n</table>\n";

  const std::vector<const CatalogEntry*> ranked = ranked_entries(catalog);
  const std::size_t shown = std::min(top, ranked.size());
  out += "<h2>Ranked ports</h2>\n<table>\n<tr><th>#</th><th>Jar</th><th>Plugin</th>"
         "<th>Status</th><th>Coverage</th><th>Score</th><th>Why</th></tr>\n";
  for (std::size_t i = 0; i < shown; ++i) {
    const CatalogEntry& entry = *ranked[i];
    const bool has_coverage = entry.status == "portable" || entry.status == "partial";
    out += "<tr><td>" + std::to_string(i + 1) + "</td><td><code>" + html_text(entry.file) +
           "</code></td><td>";
    out += entry.name.empty() ? std::string("-")
                              : html_text(entry.name) + " " + html_text(entry.version);
    out += "</td><td class=\"" + html_text(entry.status) + "\">" + html_text(entry.status) +
           "</td><td>";
    out += has_coverage ? percent_text(entry) : std::string("-");
    out += "</td><td>";
    out += entry.has_score ? std::string(score_name(entry.score)) : std::string("-");
    out += "</td><td>" + html_text(markdown_why(entry)) + "</td></tr>\n";
  }
  out += "</table>\n";
  if (shown < ranked.size()) {
    out += "<p>Showing the first " + std::to_string(shown) + " of " +
           std::to_string(ranked.size()) + " entries; <code>--top</code> controls the cap.</p>\n";
  }

  out += "<h2>What this is not</h2>\n<ul>\n";
  out += "<li><strong>Not a portability proof.</strong> <code>portable</code> means every "
         "reference the jar names has a native symbol, not that the plugin has been run "
         "against this engine.</li>\n";
  out += "<li><strong>Not a finished port.</strong> Generated handlers start from defaults; "
         "commands, permissions, scheduling and configuration remain hand work.</li>\n";
  out += "<li><strong>Coverage is the mapper's table.</strong> A mapped call can still "
         "behave differently.</li>\n</ul>\n";

  out += "<h2>Notes</h2>\n";
  if (catalog.notes.empty()) {
    out += "<p>none</p>\n";
  } else {
    out += "<ul>\n";
    for (const std::string& note : catalog.notes)
      out += "<li>" + html_text(note) + "</li>\n";
    out += "</ul>\n";
  }
  return out + "</body>\n</html>\n";
}

} // namespace mc::converter
