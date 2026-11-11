// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-convert` is the +MC Converter's command-line surface. Slice 1 is
// `analyze`: a static report over a Bukkit/Spigot/Paper plugin JAR. Slice 2 is
// `map`: the same references mapped to the native API, with explicit gaps.
// Slice 3 is `port`: a native project scaffold plus the work a human must
// finish. Slice 4 is `catalog`: a versioned index over a directory of jars,
// summarizing the analyze/map decisions for a compatibility list. Slice 5 adds
// exact-descriptor overload decisions and a `--fail-under` coverage gate to
// `map` and `catalog`. Slice 8 adds `--report`: the same scan as a ranked,
// publishable Markdown document (“the Verified list”) with a `--top` cap.
// Slice 9 makes it deployable: `--history` keeps an append-only run ledger,
// `--publish` writes the static site and its SHA256SUMS, `--sign-key` signs
// that list with Ed25519, and `verify-site` checks a published copy.
// None of them loads, links or executes plugin code.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "converter/api_map.h"
#include "converter/catalog.h"
#include "converter/jar_scan.h"
#include "converter/porter.h"
#include "converter/publish.h"

namespace {

void usage() {
  std::cout << "usage:\n"
            << "  plusmc-convert analyze <plugin.jar> [--json]\n"
            << "  plusmc-convert map <plugin.jar> [--json] [--fail-under <percent>]\n"
            << "  plusmc-convert port <plugin.jar> --out <directory> [--json] [--from-port]\n"
            << "  plusmc-convert catalog <directory> [--out <index.json>] [--json]\n"
            << "                     [--report <verified.md>] [--top <n>]\n"
            << "                     [--history <history.tsv>] [--publish <directory>]\n"
            << "                     [--sign-key <private.pem>]\n"
            << "                     [--fail-under <percent>]\n"
            << "  plusmc-convert verify-site <directory> [--key <public.pem>] [--json]\n";
}

// 101 is outside the valid 0..100 range, so it can mean "not given" without
// spending a valid percentage value on a sentinel.
constexpr unsigned kNoThreshold = 101;

bool parse_percent(const std::string& text, unsigned* value) {
  if (text.empty())
    return false;
  unsigned parsed = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    if (parsed > 100)
      return false;
  }
  *value = parsed;
  return true;
}

bool parse_threshold(const std::string& text, unsigned* fail_under, const char* option) {
  if (*fail_under != kNoThreshold) {
    std::cerr << "plusmc-convert: " << option << " given more than once\n";
    return false;
  }
  unsigned value = 0;
  if (!parse_percent(text, &value)) {
    std::cerr << "plusmc-convert: " << option << " must be a percentage from 0 to 100\n";
    return false;
  }
  *fail_under = value;
  return true;
}

// `analyze` and `map` take the same arguments: one package and an optional
// `--json`. `--fail-under` is accepted only for `map` (nullptr otherwise).
bool parse_target(int argc, char** argv, std::string* package, bool* json, unsigned* fail_under) {
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      *json = true;
    } else if (argument == "--fail-under") {
      if (fail_under == nullptr) {
        std::cerr << "plusmc-convert: unknown option " << argument << "\n";
        return false;
      }
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --fail-under needs a percentage\n";
        return false;
      }
      if (!parse_threshold(argv[++i], fail_under, "--fail-under"))
        return false;
    } else if (argument.rfind("--fail-under=", 0) == 0) {
      if (fail_under == nullptr) {
        std::cerr << "plusmc-convert: unknown option " << argument << "\n";
        return false;
      }
      if (!parse_threshold(argument.substr(13), fail_under, "--fail-under"))
        return false;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "plusmc-convert: unknown option " << argument << "\n";
      return false;
    } else if (package->empty()) {
      *package = argument;
    } else {
      std::cerr << "plusmc-convert: this command takes one package\n";
      return false;
    }
  }
  if (package->empty()) {
    usage();
    return false;
  }
  return true;
}

// `port` adds the required output directory. It accepts both `--out dir` and
// `--out=dir`, and refuses a missing or repeated option rather than guessing a
// destination for generated files.
bool parse_port(int argc, char** argv, std::string* package, std::filesystem::path* out, bool* json,
                bool* from_port) {
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      *json = true;
    } else if (argument == "--from-port") {
      *from_port = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    } else if (argument == "--out") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --out needs a directory\n";
        return false;
      }
      if (!out->empty()) {
        std::cerr << "plusmc-convert: --out given more than once\n";
        return false;
      }
      *out = argv[++i];
    } else if (argument.rfind("--out=", 0) == 0) {
      if (!out->empty()) {
        std::cerr << "plusmc-convert: --out given more than once\n";
        return false;
      }
      *out = argument.substr(6);
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "plusmc-convert: unknown option " << argument << "\n";
      return false;
    } else if (package->empty()) {
      *package = argument;
    } else {
      std::cerr << "plusmc-convert: this command takes one package\n";
      return false;
    }
  }
  if (package->empty() || out->empty()) {
    usage();
    return false;
  }
  return true;
}

bool parse_top(const std::string& text, unsigned* value) {
  if (text.empty())
    return false;
  unsigned parsed = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    if (parsed > 10000)
      return false;
  }
  if (parsed == 0)
    return false;
  *value = parsed;
  return true;
}

// `catalog` takes one directory, an optional index file and an optional
// Markdown report. Both files are written even when the directory holds
// unreadable jars, because their refusal is part of the catalog; a
// directory-level failure writes nothing.
bool parse_catalog(int argc, char** argv, std::string* directory, std::filesystem::path* out,
                   std::filesystem::path* report, bool* json, unsigned* fail_under, unsigned* top,
                   std::filesystem::path* history, std::filesystem::path* publish,
                   std::filesystem::path* sign_key) {
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      *json = true;
    } else if (argument == "--history") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --history needs a file\n";
        return false;
      }
      if (!history->empty()) {
        std::cerr << "plusmc-convert: --history given more than once\n";
        return false;
      }
      *history = argv[++i];
    } else if (argument.rfind("--history=", 0) == 0) {
      if (!history->empty()) {
        std::cerr << "plusmc-convert: --history given more than once\n";
        return false;
      }
      *history = argument.substr(10);
    } else if (argument == "--publish") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --publish needs a directory\n";
        return false;
      }
      if (!publish->empty()) {
        std::cerr << "plusmc-convert: --publish given more than once\n";
        return false;
      }
      *publish = argv[++i];
    } else if (argument.rfind("--publish=", 0) == 0) {
      if (!publish->empty()) {
        std::cerr << "plusmc-convert: --publish given more than once\n";
        return false;
      }
      *publish = argument.substr(10);
    } else if (argument == "--sign-key") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --sign-key needs a file\n";
        return false;
      }
      if (!sign_key->empty()) {
        std::cerr << "plusmc-convert: --sign-key given more than once\n";
        return false;
      }
      *sign_key = argv[++i];
    } else if (argument.rfind("--sign-key=", 0) == 0) {
      if (!sign_key->empty()) {
        std::cerr << "plusmc-convert: --sign-key given more than once\n";
        return false;
      }
      *sign_key = argument.substr(11);
    } else if (argument == "--report") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --report needs a file\n";
        return false;
      }
      if (!report->empty()) {
        std::cerr << "plusmc-convert: --report given more than once\n";
        return false;
      }
      *report = argv[++i];
    } else if (argument.rfind("--report=", 0) == 0) {
      if (!report->empty()) {
        std::cerr << "plusmc-convert: --report given more than once\n";
        return false;
      }
      *report = argument.substr(9);
    } else if (argument == "--top") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --top needs a count\n";
        return false;
      }
      if (*top != 0) {
        std::cerr << "plusmc-convert: --top given more than once\n";
        return false;
      }
      if (!parse_top(argv[++i], top)) {
        std::cerr << "plusmc-convert: --top must be a whole number from 1 to 10000\n";
        return false;
      }
    } else if (argument.rfind("--top=", 0) == 0) {
      if (*top != 0) {
        std::cerr << "plusmc-convert: --top given more than once\n";
        return false;
      }
      if (!parse_top(argument.substr(6), top)) {
        std::cerr << "plusmc-convert: --top must be a whole number from 1 to 10000\n";
        return false;
      }
    } else if (argument == "--fail-under") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --fail-under needs a percentage\n";
        return false;
      }
      if (!parse_threshold(argv[++i], fail_under, "--fail-under"))
        return false;
    } else if (argument.rfind("--fail-under=", 0) == 0) {
      if (!parse_threshold(argument.substr(13), fail_under, "--fail-under"))
        return false;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    } else if (argument == "--out") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --out needs a file\n";
        return false;
      }
      if (!out->empty()) {
        std::cerr << "plusmc-convert: --out given more than once\n";
        return false;
      }
      *out = argv[++i];
    } else if (argument.rfind("--out=", 0) == 0) {
      if (!out->empty()) {
        std::cerr << "plusmc-convert: --out given more than once\n";
        return false;
      }
      *out = argument.substr(6);
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "plusmc-convert: unknown option " << argument << "\n";
      return false;
    } else if (directory->empty()) {
      *directory = argument;
    } else {
      std::cerr << "plusmc-convert: this command takes one directory\n";
      return false;
    }
  }
  if (directory->empty()) {
    usage();
    return false;
  }
  if (report->empty() && publish->empty() && *top != 0) {
    std::cerr << "plusmc-convert: --top needs --report or --publish\n";
    return false;
  }
  if ((!report->empty() || !publish->empty()) && *top == 0)
    *top = mc::converter::kDefaultCatalogReportTop;
  // A signature without a site would have nothing to sign; refusing is
  // clearer than silently ignoring the key.
  if (!sign_key->empty() && publish->empty()) {
    std::cerr << "plusmc-convert: --sign-key needs --publish\n";
    return false;
  }
  return true;
}

bool parse_verify(int argc, char** argv, std::string* directory, std::filesystem::path* key,
                  bool* json) {
  for (int i = 2; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--json") {
      *json = true;
    } else if (argument == "--key") {
      if (i + 1 >= argc) {
        std::cerr << "plusmc-convert: --key needs a public key file\n";
        return false;
      }
      if (!key->empty()) {
        std::cerr << "plusmc-convert: --key given more than once\n";
        return false;
      }
      *key = argv[++i];
    } else if (argument.rfind("--key=", 0) == 0) {
      if (!key->empty()) {
        std::cerr << "plusmc-convert: --key given more than once\n";
        return false;
      }
      *key = argument.substr(6);
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return false;
    } else if (!argument.empty() && argument.front() == '-') {
      std::cerr << "plusmc-convert: unknown option " << argument << "\n";
      return false;
    } else if (directory->empty()) {
      *directory = argument;
    } else {
      std::cerr << "plusmc-convert: this command takes one directory\n";
      return false;
    }
  }
  if (directory->empty()) {
    usage();
    return false;
  }
  return true;
}

// One file, created with its parents, with one write and one check.
bool write_text(const std::filesystem::path& path, const std::string& text) {
  std::error_code error;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    std::cerr << "plusmc-convert: cannot create " << path.parent_path().string() << ": "
              << error.message() << "\n";
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    std::cerr << "plusmc-convert: cannot write " << path.string() << "\n";
    return false;
  }
  file << text;
  file.close();
  if (!file) {
    std::cerr << "plusmc-convert: cannot write " << path.string() << "\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command != "analyze" && command != "map" && command != "port" && command != "catalog" &&
      command != "verify-site") {
    usage();
    return 2;
  }

  if (command == "verify-site") {
    std::string directory;
    std::filesystem::path key;
    bool json = false;
    if (!parse_verify(argc, argv, &directory, &key, &json))
      return 2;
    const mc::converter::VerifyReport verified = mc::converter::verify_site(directory, key);
    std::cout << (json ? mc::converter::verify_report_json(verified)
                       : mc::converter::verify_report_text(verified));
    if (!verified.ok) {
      std::cerr << "plusmc-convert: " << verified.error << "\n";
      return 1;
    }
    return 0;
  }

  if (command == "catalog") {
    std::string directory;
    std::filesystem::path out;
    std::filesystem::path report;
    std::filesystem::path history;
    std::filesystem::path publish;
    std::filesystem::path sign_key;
    bool json = false;
    unsigned fail_under = kNoThreshold;
    unsigned top = 0;
    if (!parse_catalog(argc, argv, &directory, &out, &report, &json, &fail_under, &top, &history,
                       &publish, &sign_key))
      return 2;
    mc::converter::Catalog catalog = mc::converter::build_catalog(directory);
    // A publish without an explicit ledger keeps one beside the site, so a
    // deployed catalog always carries its own run history.
    if (catalog.ok) {
      const std::filesystem::path ledger =
          history.empty() && !publish.empty() ? publish / "history.tsv" : history;
      if (!ledger.empty()) {
        mc::converter::HistoryDelta delta = mc::converter::append_history(ledger, catalog);
        if (!delta.ok) {
          std::cerr << "plusmc-convert: " << delta.error << "\n";
          return 1;
        }
        catalog.history = std::move(delta);
      }
    }
    const std::string json_text = mc::converter::catalog_json(catalog);
    // The index is the machine artifact; the report is the one a human
    // publishes. Both are written before the coverage gate can fail, so CI can
    // keep the artifact of the run that failed.
    if (!out.empty() && catalog.ok && !write_text(out, json_text))
      return 1;
    if (!report.empty() && catalog.ok &&
        !write_text(report, mc::converter::catalog_markdown(catalog, top)))
      return 1;
    std::string published;
    if (!publish.empty()) {
      if (!catalog.ok) {
        std::cerr << "plusmc-convert: " << catalog.error << "\n";
        return 1;
      }
      const mc::converter::SiteReport site =
          mc::converter::publish_site(publish, catalog, top, sign_key);
      if (!site.ok) {
        std::cerr << "plusmc-convert: " << site.error << "\n";
        return 1;
      }
      published = mc::converter::site_report_text(site);
    }
    if (json) {
      std::cout << json_text;
    } else {
      std::cout << mc::converter::catalog_text(catalog);
      if (catalog.history)
        std::cout << mc::converter::history_text(*catalog.history);
      std::cout << published;
    }
    if (!catalog.ok) {
      std::cerr << "plusmc-convert: " << catalog.error << "\n";
      return 1;
    }
    if (fail_under != kNoThreshold) {
      for (const mc::converter::CatalogEntry& entry : catalog.entries) {
        // Refused and unreadable jars carry no coverage claim; gating them
        // would just restate that they cannot be ported.
        if (entry.status != "portable" && entry.status != "partial")
          continue;
        const std::size_t total = entry.mapped + entry.partial + entry.gap;
        const std::size_t covered = entry.mapped + entry.partial;
        const bool meets = total == 0
                               ? fail_under == 0
                               : covered * 100 >= static_cast<std::size_t>(fail_under) * total;
        if (meets)
          continue;
        const unsigned percent = total == 0 ? 0 : static_cast<unsigned>((covered * 100) / total);
        std::cerr << "plusmc-convert: " << entry.file << " coverage " << percent
                  << "% is below --fail-under " << fail_under << "%\n";
        return 1;
      }
    }
    return 0;
  }

  if (command == "port") {
    std::string package;
    std::filesystem::path out;
    bool json = false;
    bool from_port = false;
    if (!parse_port(argc, argv, &package, &out, &json, &from_port))
      return 2;
    mc::converter::PortOptions options;
    options.package = package;
    options.out_dir = out;
    options.from_port = from_port;
    const mc::converter::PortReport report = mc::converter::port_jar(options);
    std::cout << (json ? mc::converter::port_report_json(report)
                       : mc::converter::port_report_text(report));
    if (!report.ok) {
      std::cerr << "plusmc-convert: " << report.error << "\n";
      return 1;
    }
    return 0;
  }

  std::string package;
  bool json = false;
  unsigned fail_under = kNoThreshold;
  if (!parse_target(argc, argv, &package, &json, command == "map" ? &fail_under : nullptr))
    return 2;

  const mc::converter::Analysis analysis = mc::converter::analyze_jar(package);
  if (command == "analyze") {
    std::cout << (json ? mc::converter::report_json(analysis)
                       : mc::converter::report_text(analysis));
    if (!analysis.ok) {
      std::cerr << "plusmc-convert: " << analysis.error << "\n";
      return 1;
    }
    return 0;
  }

  const mc::converter::MappingReport report = mc::converter::map_analysis(analysis);
  std::cout << (json ? mc::converter::map_report_json(report)
                     : mc::converter::map_report_text(report));
  if (!report.ok) {
    std::cerr << "plusmc-convert: " << report.error << "\n";
    return 1;
  }
  if (fail_under != kNoThreshold) {
    const std::size_t total = mc::converter::mapping_reference_count(report);
    const std::size_t covered = mc::converter::mapping_covered_count(report);
    const bool meets = total == 0 ? fail_under == 0
                                  : covered * 100 >= static_cast<std::size_t>(fail_under) * total;
    if (!meets) {
      const unsigned percent = total == 0 ? 0 : static_cast<unsigned>((covered * 100) / total);
      std::cerr << "plusmc-convert: coverage " << percent << "% is below --fail-under "
                << fail_under << "%\n";
      return 1;
    }
  }
  return 0;
}
