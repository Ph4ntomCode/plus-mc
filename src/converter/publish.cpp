// SPDX-License-Identifier: GPL-3.0-or-later
//
// The deployment half of the Verified catalog. `publish.cpp` owns three files
// the rest of the tool only consumes:
//
//   history.tsv   the append-only ledger (one row per jar per run)
//   SHA256SUMS    the integrity list over the published artifacts
//   verified.sig  an Ed25519 signature over the exact SHA256SUMS bytes
//
// The ledger is TSV rather than JSON on purpose: a run is a handful of rows,
// a review of a score change is a diff of those rows, and reading the previous
// run therefore needs no JSON parser. Every field is tab-cleaned when written
// and the reader refuses a row it does not understand instead of guessing.

#include "converter/publish.h"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "converter/report_json.h"

namespace mc::converter {
namespace {

constexpr std::size_t kMaxLedgerBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxSumFileBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxArtifactBytes = 512u * 1024u * 1024u;
constexpr std::size_t kMaxSignatureBytes = 4096;

const char* kHistoryHeader =
    "# +MC Verified catalog history (schema 1)\n"
    "# One row per jar per run. Re-publishing an unchanged scan adds no run.\n"
    "# run\tdigest\tdirectory\tfile\tstatus\tscore\tcoverage\tname\tversion\n";

bool is_hex_digest(const std::string& value) {
  if (value.size() != 64)
    return false;
  for (const char c : value) {
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    if (!digit && !lower)
      return false;
  }
  return true;
}

// A tab, newline or carriage return in a plugin name would split a ledger row
// or forge one; the writer keeps every field single-line.
std::string clean_field(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c == '\t' || c == '\n' || c == '\r' || c == '\0')
      out.push_back(' ');
    else
      out.push_back(c);
  }
  return out;
}

std::string sha256_hex(const std::string& bytes) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr)
    return {};
  const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1 &&
                  EVP_DigestFinal_ex(context, digest.data(), &length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok || length != 32)
    return {};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned int i = 0; i < length; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 0x0f]);
  }
  return out;
}

std::string base64_encode(const std::vector<uint8_t>& bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const std::size_t remaining = bytes.size() - i;
    const uint32_t first = bytes[i];
    const uint32_t second = remaining > 1 ? bytes[i + 1] : 0;
    const uint32_t third = remaining > 2 ? bytes[i + 2] : 0;
    const uint32_t group = (first << 16) | (second << 8) | third;
    out.push_back(kAlphabet[(group >> 18) & 0x3f]);
    out.push_back(kAlphabet[(group >> 12) & 0x3f]);
    out.push_back(remaining > 1 ? kAlphabet[(group >> 6) & 0x3f] : '=');
    out.push_back(remaining > 2 ? kAlphabet[group & 0x3f] : '=');
  }
  return out;
}

int base64_value(const char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  std::string compact;
  compact.reserve(text.size());
  for (const char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
      continue;
    compact.push_back(c);
  }
  if (compact.empty() || compact.size() % 4 != 0)
    return false;
  out->clear();
  out->reserve(compact.size() / 4 * 3);
  for (std::size_t i = 0; i < compact.size(); i += 4) {
    int values[4] = {0, 0, 0, 0};
    int padding = 0;
    for (int j = 0; j < 4; ++j) {
      const char c = compact[i + static_cast<std::size_t>(j)];
      if (c == '=') {
        // Padding is only legal in the last group's last two positions.
        if (i + 4 != compact.size() || j < 2)
          return false;
        ++padding;
        values[j] = 0;
        continue;
      }
      if (padding != 0)
        return false;
      const int value = base64_value(c);
      if (value < 0)
        return false;
      values[j] = value;
    }
    const uint32_t group =
        (static_cast<uint32_t>(values[0]) << 18) | (static_cast<uint32_t>(values[1]) << 12) |
        (static_cast<uint32_t>(values[2]) << 6) | static_cast<uint32_t>(values[3]);
    out->push_back(static_cast<uint8_t>((group >> 16) & 0xff));
    if (padding < 2)
      out->push_back(static_cast<uint8_t>((group >> 8) & 0xff));
    if (padding < 1)
      out->push_back(static_cast<uint8_t>(group & 0xff));
  }
  return true;
}

bool read_file(const std::filesystem::path& path, std::size_t cap, std::string* text,
               std::string* error) {
  std::error_code status_error;
  const std::uintmax_t size = std::filesystem::file_size(path, status_error);
  if (status_error) {
    *error = "cannot read " + path.generic_string() + ": " + status_error.message();
    return false;
  }
  if (size > cap) {
    *error = path.generic_string() + " is larger than the " + std::to_string(cap) + "-byte limit";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (!file.good() && !file.eof()) {
    *error = "cannot read " + path.generic_string();
    return false;
  }
  *text = buffer.str();
  return true;
}

// Write-through-temporary: a crash leaves the old ledger or the new one, never
// half of either.
bool write_file(const std::filesystem::path& path, const std::string& text, std::string* error) {
  std::error_code directory_error;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
      *error =
          "cannot create " + path.parent_path().generic_string() + ": " + directory_error.message();
      return false;
    }
  }
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      *error = "cannot write " + temporary.generic_string();
      return false;
    }
    file << text;
    file.close();
    if (!file) {
      *error = "cannot write " + temporary.generic_string();
      return false;
    }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, path, rename_error);
  if (rename_error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    *error = "cannot write " + path.generic_string() + ": " + rename_error.message();
    return false;
  }
  return true;
}

std::string absolute_text(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal().generic_string();
}

struct LedgerRow {
  unsigned run = 0;
  std::string digest;
  std::string directory;
  std::string file;
  std::string status;
  std::string score;
  std::string coverage;
  std::string name;
  std::string version;
};

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  for (const char c : line) {
    if (c == '\t') {
      fields.push_back(field);
      field.clear();
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(field);
  return fields;
}

bool parse_run(const std::string& text, unsigned* run) {
  if (text.empty() || text.size() > 10)
    return false;
  unsigned value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value == 0)
    return false;
  *run = value;
  return true;
}

bool valid_coverage(const std::string& text) {
  // The writer's exact `%.3f` shape: digits, a dot, three digits.
  const std::size_t dot = text.find('.');
  if (dot == std::string::npos || dot == 0 || text.size() != dot + 4)
    return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == dot)
      continue;
    if (text[i] < '0' || text[i] > '9')
      return false;
  }
  return true;
}

bool read_ledger(const std::filesystem::path& path, std::vector<LedgerRow>* rows,
                 std::string* error) {
  rows->clear();
  std::string text;
  if (!read_file(path, kMaxLedgerBytes, &text, error))
    return false;
  std::istringstream stream(text);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    const std::vector<std::string> fields = split_tabs(line);
    if (fields.size() != 9) {
      *error = path.generic_string() + ":" + std::to_string(line_number) +
               ": expected 9 tab-separated fields, found " + std::to_string(fields.size());
      return false;
    }
    LedgerRow row;
    if (!parse_run(fields[0], &row.run)) {
      *error = path.generic_string() + ":" + std::to_string(line_number) +
               ": the run column is not a positive whole number";
      return false;
    }
    if (!is_hex_digest(fields[1])) {
      *error = path.generic_string() + ":" + std::to_string(line_number) +
               ": the digest column is not 64 lowercase hex characters";
      return false;
    }
    if (fields[4] != "portable" && fields[4] != "partial" && fields[4] != "refused" &&
        fields[4] != "unreadable") {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": unknown status \"" +
               fields[4] + "\"";
      return false;
    }
    if (fields[5] != "green" && fields[5] != "yellow" && fields[5] != "red" && fields[5] != "-") {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": unknown score \"" +
               fields[5] + "\"";
      return false;
    }
    if (!valid_coverage(fields[6])) {
      *error = path.generic_string() + ":" + std::to_string(line_number) + ": coverage \"" +
               fields[6] + "\" is not a 0.000-1.000 ratio";
      return false;
    }
    row.digest = fields[1];
    row.directory = fields[2];
    row.file = fields[3];
    row.status = fields[4];
    row.score = fields[5];
    row.coverage = fields[6];
    row.name = fields[7];
    row.version = fields[8];
    rows->push_back(std::move(row));
  }
  return true;
}

std::string ledger_text(const std::vector<LedgerRow>& rows) {
  std::string out = kHistoryHeader;
  for (const LedgerRow& row : rows) {
    out += std::to_string(row.run) + "\t" + row.digest + "\t" + row.directory + "\t" + row.file +
           "\t" + row.status + "\t" + row.score + "\t" + row.coverage + "\t" + row.name + "\t" +
           row.version + "\n";
  }
  return out;
}

std::string score_text(const CatalogEntry& entry) {
  return entry.has_score ? std::string(score_name(entry.score)) : std::string("-");
}

std::string coverage_field(const CatalogEntry& entry) {
  char text[32];
  const std::size_t total = entry.mapped + entry.partial + entry.gap;
  const double ratio = total == 0 ? 0.0 : static_cast<double>(entry.mapped + entry.partial) / total;
  std::snprintf(text, sizeof(text), "%.3f", ratio);
  return text;
}

HistoryEntry history_entry(const CatalogEntry& entry) {
  HistoryEntry out;
  out.file = entry.file;
  out.name = entry.name;
  out.version = entry.version;
  out.status = entry.status;
  out.score = score_text(entry);
  out.coverage = coverage_field(entry);
  return out;
}

HistoryEntry history_entry_from_row(const LedgerRow& row) {
  HistoryEntry out;
  out.file = row.file;
  out.name = row.name;
  out.version = row.version;
  out.status = row.status;
  out.score = row.score;
  out.coverage = row.coverage;
  return out;
}

// The delta between a previous run's rows and this scan. An added file, a lost
// file, and a file whose status, score, coverage, name or version moved.
void fill_delta(const std::vector<LedgerRow>& previous, const Catalog& catalog,
                HistoryDelta* delta) {
  std::map<std::string, const LedgerRow*> before;
  for (const LedgerRow& row : previous)
    before[row.file] = &row;
  std::set<std::string> after;
  for (const CatalogEntry& entry : catalog.entries)
    after.insert(entry.file);

  for (const CatalogEntry& entry : catalog.entries) {
    const HistoryEntry current = history_entry(entry);
    const auto found = before.find(entry.file);
    if (found == before.end()) {
      delta->added.push_back(current);
      continue;
    }
    const LedgerRow& old = *found->second;
    if (old.status == current.status && old.score == current.score &&
        old.coverage == current.coverage && old.name == current.name &&
        old.version == current.version) {
      ++delta->unchanged;
      continue;
    }
    HistoryChange change;
    change.entry = current;
    change.from_status = old.status;
    change.to_status = current.status;
    change.from_coverage = old.coverage;
    change.to_coverage = current.coverage;
    delta->changed.push_back(std::move(change));
  }
  for (const LedgerRow& row : previous) {
    if (after.count(row.file) == 0)
      delta->removed.push_back(history_entry_from_row(row));
  }
}

// The scan's identity: the decisions, not the absolute directory. Two machines
// scanning the same jars produce the same digest, so a history compared across
// a laptop and CI is meaningful.
std::string scan_digest(const Catalog& catalog) {
  std::string canonical = "plusmc-verified-catalog\t1\n";
  for (const CatalogEntry& entry : catalog.entries) {
    canonical += clean_field(entry.file) + "\t" + clean_field(entry.name) + "\t" +
                 clean_field(entry.version) + "\t" + clean_field(entry.api_version) + "\t" +
                 entry.status + "\t" + score_text(entry) + "\t" + std::to_string(entry.mapped) +
                 "\t" + std::to_string(entry.partial) + "\t" + std::to_string(entry.gap) + "\n";
  }
  return sha256_hex(canonical);
}

std::vector<LedgerRow> rows_for(const Catalog& catalog, unsigned run, const std::string& digest) {
  std::vector<LedgerRow> rows;
  rows.reserve(catalog.entries.size());
  for (const CatalogEntry& entry : catalog.entries) {
    LedgerRow row;
    row.run = run;
    row.digest = digest;
    row.directory = clean_field(catalog.directory);
    row.file = clean_field(entry.file);
    row.status = entry.status;
    row.score = score_text(entry);
    row.coverage = coverage_field(entry);
    row.name = clean_field(entry.name);
    row.version = clean_field(entry.version);
    rows.push_back(std::move(row));
  }
  return rows;
}

// Ed25519 is the only key type the site signature accepts; the module packer
// makes the same restriction, so one key works for both products.
bool read_private_key(const std::filesystem::path& path, EVP_PKEY** key, std::string* error) {
  FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    *error = "cannot read signing key " + path.generic_string();
    return false;
  }
  *key = PEM_read_PrivateKey(file, nullptr, nullptr, nullptr);
  std::fclose(file);
  if (*key == nullptr) {
    *error = "cannot parse signing key " + path.generic_string();
    return false;
  }
  if (EVP_PKEY_base_id(*key) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(*key);
    *key = nullptr;
    *error = "signing key is not Ed25519";
    return false;
  }
  return true;
}

bool sign_bytes(EVP_PKEY* key, const std::string& bytes, std::vector<uint8_t>* signature,
                std::string* error) {
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    *error = "OpenSSL is unavailable";
    return false;
  }
  std::size_t length = 0;
  bool ok = EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestSign(context, nullptr, &length,
                           reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) == 1;
  signature->assign(length, 0);
  if (ok)
    ok = EVP_DigestSign(context, signature->data(), &length,
                        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) == 1;
  EVP_MD_CTX_free(context);
  if (!ok) {
    *error = "cannot sign SHA256SUMS";
    return false;
  }
  signature->resize(length);
  return true;
}

// The public key is emitted as PEM text rather than written straight to disk,
// so it is hashed and listed in SHA256SUMS like every other artifact.
bool public_key_pem(EVP_PKEY* key, std::string* out, std::string* error) {
  BIO* buffer = BIO_new(BIO_s_mem());
  if (buffer == nullptr) {
    *error = "OpenSSL is unavailable";
    return false;
  }
  const bool ok = PEM_write_bio_PUBKEY(buffer, key) == 1;
  char* data = nullptr;
  const long size = ok ? BIO_get_mem_data(buffer, &data) : 0;
  if (!ok || data == nullptr || size <= 0) {
    BIO_free(buffer);
    *error = "cannot encode the public key";
    return false;
  }
  out->assign(data, static_cast<std::size_t>(size));
  BIO_free(buffer);
  return true;
}

} // namespace

HistoryDelta append_history(const std::filesystem::path& path, const Catalog& catalog) {
  HistoryDelta delta;
  delta.file = absolute_text(path);

  std::vector<LedgerRow> rows;
  std::error_code exists_error;
  const bool exists = std::filesystem::exists(path, exists_error);
  if (exists_error) {
    delta.error = "cannot inspect " + delta.file + ": " + exists_error.message();
    return delta;
  }
  if (exists) {
    std::string error;
    if (!read_ledger(path, &rows, &error)) {
      delta.error = error;
      return delta;
    }
  }

  unsigned latest = 0;
  for (const LedgerRow& row : rows)
    latest = std::max(latest, row.run);
  const std::string digest = scan_digest(catalog);
  if (digest.empty()) {
    delta.error = "cannot hash the scan";
    return delta;
  }

  // The latest run is the baseline, whether or not this scan appends a run.
  std::vector<LedgerRow> previous;
  for (const LedgerRow& row : rows) {
    if (row.run == latest)
      previous.push_back(row);
  }
  if (latest != 0 && previous.front().digest == digest) {
    // Reproduce the delta that run recorded when it was created, by comparing
    // it with the run before it. That keeps `index.json` byte-identical on a
    // re-publish instead of turning "what changed" into "nothing changed".
    unsigned earlier = 0;
    for (const LedgerRow& row : rows) {
      if (row.run < latest)
        earlier = std::max(earlier, row.run);
    }
    std::vector<LedgerRow> before;
    for (const LedgerRow& row : rows) {
      if (row.run == earlier)
        before.push_back(row);
    }
    fill_delta(before, catalog, &delta);
    delta.ok = true;
    delta.run = latest;
    delta.appended = false;
    return delta;
  }

  fill_delta(previous, catalog, &delta);

  const unsigned run = latest + 1;
  const std::vector<LedgerRow> fresh = rows_for(catalog, run, digest);
  rows.insert(rows.end(), fresh.begin(), fresh.end());

  // Keep the newest `kMaxHistoryRuns` runs. The ledger is a review artifact,
  // not an archive; the published index for each run carries the detail.
  std::vector<unsigned> runs;
  for (const LedgerRow& row : rows)
    runs.push_back(row.run);
  std::sort(runs.begin(), runs.end());
  runs.erase(std::unique(runs.begin(), runs.end()), runs.end());
  if (runs.size() > kMaxHistoryRuns) {
    const unsigned oldest_kept = runs[runs.size() - kMaxHistoryRuns];
    rows.erase(
        std::remove_if(rows.begin(), rows.end(),
                       [oldest_kept](const LedgerRow& row) { return row.run < oldest_kept; }),
        rows.end());
  }

  std::string error;
  if (!write_file(path, ledger_text(rows), &error)) {
    delta.error = error;
    return delta;
  }

  delta.ok = true;
  delta.run = run;
  delta.appended = true;
  return delta;
}

std::string history_text(const HistoryDelta& delta) {
  if (!delta.ok)
    return "history: " + (delta.error.empty() ? std::string("failed") : delta.error) + "\n";

  std::string out = "history: ";
  if (delta.appended) {
    out += "run " + std::to_string(delta.run) + " appended to " + delta.file + " (" +
           std::to_string(delta.added.size()) + " added, " + std::to_string(delta.removed.size()) +
           " removed, " + std::to_string(delta.changed.size()) + " changed, " +
           std::to_string(delta.unchanged) + " unchanged)\n";
  } else {
    out += "run " + std::to_string(delta.run) + " already describes this scan; " + delta.file +
           " is unchanged\n";
  }

  constexpr std::size_t kDisplayed = 40;
  std::size_t shown = 0;
  auto line = [&](const std::string& text) {
    if (shown < kDisplayed) {
      out += "  " + text + "\n";
      ++shown;
    }
  };
  for (const HistoryEntry& entry : delta.added)
    line("+ " + entry.file + "  " + entry.status + "  " + entry.coverage);
  for (const HistoryEntry& entry : delta.removed)
    line("- " + entry.file);
  for (const HistoryChange& change : delta.changed) {
    if (change.from_status != change.to_status) {
      line("~ " + change.entry.file + "  " + change.from_status + " -> " + change.to_status +
           "  (" + change.from_coverage + " -> " + change.to_coverage + ")");
    } else {
      line("~ " + change.entry.file + "  " + change.from_coverage + " -> " + change.to_coverage);
    }
  }
  const std::size_t total = delta.added.size() + delta.removed.size() + delta.changed.size();
  if (total > shown)
    out += "  ... and " + std::to_string(total - shown) + " more\n";
  return out;
}

SiteReport publish_site(const std::filesystem::path& directory, const Catalog& catalog,
                        std::size_t top, const std::filesystem::path& signing_key) {
  SiteReport report;
  report.directory = absolute_text(directory);
  if (!catalog.ok) {
    report.error = "the scan failed: " + catalog.error;
    return report;
  }
  if (!catalog.history || !catalog.history->ok) {
    report.error = "publishing needs a recorded history run";
    return report;
  }

  std::error_code status_error;
  if (std::filesystem::exists(directory, status_error) &&
      !std::filesystem::is_directory(directory, status_error)) {
    report.error = "not a directory: " + report.directory;
    return report;
  }
  std::error_code create_error;
  std::filesystem::create_directories(directory, create_error);
  if (create_error) {
    report.error = "cannot create " + report.directory + ": " + create_error.message();
    return report;
  }

  std::string ledger;
  std::string error;
  if (!read_file(catalog.history->file, kMaxLedgerBytes, &ledger, &error)) {
    report.error = error;
    return report;
  }

  std::vector<std::pair<std::string, std::string>> artifacts;
  artifacts.emplace_back("index.json", catalog_json(catalog));
  artifacts.emplace_back("verified.md", catalog_markdown(catalog, top));
  artifacts.emplace_back("history.tsv", ledger);
  artifacts.emplace_back("index.html", catalog_html(catalog, top));
  std::sort(artifacts.begin(), artifacts.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });

  std::string sums;
  for (const auto& [name, text] : artifacts) {
    const std::string hash = sha256_hex(text);
    if (hash.empty()) {
      report.error = "cannot hash " + name;
      return report;
    }
    sums += hash + "  " + name + "\n";
  }

  // Signing happens before the first write: a key that does not parse leaves
  // the previous site untouched instead of half-replaced.
  std::string signature_text;
  std::string pem;
  if (!signing_key.empty()) {
    EVP_PKEY* key = nullptr;
    if (!read_private_key(signing_key, &key, &error)) {
      report.error = error;
      return report;
    }
    std::vector<uint8_t> signature;
    const bool signed_ok = sign_bytes(key, sums, &signature, &error);
    const bool encoded_public = signed_ok && public_key_pem(key, &pem, &error);
    EVP_PKEY_free(key);
    if (!signed_ok || !encoded_public) {
      report.error = error;
      return report;
    }
    signature_text = base64_encode(signature) + "\n";
  }

  for (const auto& [name, text] : artifacts) {
    if (!write_file(directory / name, text, &error)) {
      report.error = error;
      return report;
    }
    report.files.push_back(SiteFile{name, sha256_hex(text), text.size()});
  }
  if (!write_file(directory / "SHA256SUMS", sums, &error)) {
    report.error = error;
    return report;
  }
  report.files.push_back(SiteFile{"SHA256SUMS", sha256_hex(sums), sums.size()});

  if (!signing_key.empty()) {
    if (!write_file(directory / "verified.sig", signature_text, &error) ||
        !write_file(directory / "verified.pub.pem", pem, &error)) {
      report.error = error;
      return report;
    }
    report.files.push_back(
        SiteFile{"verified.sig", sha256_hex(signature_text), signature_text.size()});
    report.files.push_back(SiteFile{"verified.pub.pem", sha256_hex(pem), pem.size()});
    report.signed_site = true;
  }

  report.ok = true;
  return report;
}

VerifyReport verify_site(const std::filesystem::path& directory, const std::filesystem::path& key) {
  VerifyReport report;
  report.directory = absolute_text(directory);
  std::error_code status_error;
  if (!std::filesystem::is_directory(directory, status_error)) {
    report.error = "not a directory: " + report.directory;
    return report;
  }

  std::string sums;
  std::string error;
  if (!read_file(directory / "SHA256SUMS", kMaxSumFileBytes, &sums, &error)) {
    report.error = error;
    return report;
  }

  std::vector<std::pair<std::string, std::string>> listed;
  std::set<std::string> names;
  std::istringstream stream(sums);
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    if (line.size() < 67 || line[64] != ' ' || line[65] != ' ') {
      report.error =
          "SHA256SUMS:" + std::to_string(line_number) + ": expected \"<sha256>  <name>\"";
      return report;
    }
    const std::string hash = line.substr(0, 64);
    const std::string name = line.substr(66);
    if (!is_hex_digest(hash)) {
      report.error = "SHA256SUMS:" + std::to_string(line_number) +
                     ": the first column is not 64 lowercase hex characters";
      return report;
    }
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
      report.error = "SHA256SUMS:" + std::to_string(line_number) +
                     ": the name must be a plain file in the site directory";
      return report;
    }
    if (!names.insert(name).second) {
      report.error = "SHA256SUMS lists " + name + " more than once";
      return report;
    }
    listed.emplace_back(name, hash);
  }
  if (listed.empty()) {
    report.error = "SHA256SUMS lists no files";
    return report;
  }
  for (const char* required : {"index.json", "verified.md", "history.tsv", "index.html"}) {
    if (names.count(required) == 0) {
      report.error = std::string("SHA256SUMS does not cover ") + required;
      return report;
    }
  }

  for (const auto& [name, hash] : listed) {
    std::string bytes;
    if (!read_file(directory / name, kMaxArtifactBytes, &bytes, &error)) {
      report.error = error;
      return report;
    }
    if (sha256_hex(bytes) != hash) {
      report.error = name + " does not match its SHA256SUMS entry";
      return report;
    }
    ++report.checked;
  }
  report.notes.push_back(std::to_string(report.checked) + " file(s) match SHA256SUMS");

  if (!key.empty()) {
    std::string signature_text;
    if (!read_file(directory / "verified.sig", kMaxSignatureBytes, &signature_text, &error)) {
      report.error = error;
      return report;
    }
    std::vector<uint8_t> signature;
    if (!base64_decode(signature_text, &signature)) {
      report.error = "verified.sig is not base64";
      return report;
    }
    FILE* file = std::fopen(key.string().c_str(), "rb");
    if (file == nullptr) {
      report.error = "cannot read public key " + key.generic_string();
      return report;
    }
    EVP_PKEY* public_key = PEM_read_PUBKEY(file, nullptr, nullptr, nullptr);
    std::fclose(file);
    if (public_key == nullptr) {
      report.error = "cannot parse public key " + key.generic_string();
      return report;
    }
    if (EVP_PKEY_base_id(public_key) != EVP_PKEY_ED25519) {
      EVP_PKEY_free(public_key);
      report.error = "public key is not Ed25519";
      return report;
    }
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    bool ok = context != nullptr &&
              EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, public_key) == 1 &&
              EVP_DigestVerify(context, signature.data(), signature.size(),
                               reinterpret_cast<const uint8_t*>(sums.data()), sums.size()) == 1;
    EVP_MD_CTX_free(context);
    EVP_PKEY_free(public_key);
    if (!ok) {
      report.error = "verified.sig does not sign SHA256SUMS with " + key.generic_string();
      return report;
    }
    report.signature_checked = true;
    report.notes.push_back("signature verified with " + key.generic_string());
  }

  report.ok = true;
  return report;
}

std::string site_report_text(const SiteReport& report) {
  std::string out = "+MC Verified catalog publish\n";
  out += "directory: " + report.directory + "\n";
  if (!report.ok)
    return out + "error: " + report.error + "\n";
  out += std::string("signed: ") + (report.signed_site ? "yes" : "no") + "\n";
  for (const SiteFile& file : report.files)
    out += "  " + file.name + "  " + std::to_string(file.size) + " bytes  " + file.sha256 + "\n";
  return out;
}

std::string site_report_json(const SiteReport& report) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"mode\":\"publish\",\"ok\":";
  out += report.ok ? "true" : "false";
  out += ",\"directory\":" + detail::json_quote(report.directory);
  if (!report.ok)
    return out + ",\"error\":" + detail::json_quote(report.error) + "}\n";
  out += ",\"signed\":";
  out += report.signed_site ? "true" : "false";
  out += ",\"files\":[";
  for (std::size_t i = 0; i < report.files.size(); ++i) {
    if (i != 0)
      out += ",";
    out += "{\"name\":" + detail::json_quote(report.files[i].name) +
           ",\"sha256\":" + detail::json_quote(report.files[i].sha256) +
           ",\"size\":" + std::to_string(report.files[i].size) + "}";
  }
  return out + "]}\n";
}

std::string verify_report_text(const VerifyReport& report) {
  std::string out = "+MC Verified catalog check\n";
  out += "directory: " + report.directory + "\n";
  if (!report.ok)
    return out + "error: " + report.error + "\n";
  out += "files: " + std::to_string(report.checked) + " checked\n";
  out +=
      std::string("signature: ") + (report.signature_checked ? "verified" : "not checked") + "\n";
  for (const std::string& note : report.notes)
    out += "note: " + note + "\n";
  return out;
}

std::string verify_report_json(const VerifyReport& report) {
  std::string out = "{\"tool\":\"plusmc-convert\",\"schema\":1,\"mode\":\"verify-site\",\"ok\":";
  out += report.ok ? "true" : "false";
  out += ",\"directory\":" + detail::json_quote(report.directory);
  if (!report.ok)
    return out + ",\"error\":" + detail::json_quote(report.error) + "}\n";
  out += ",\"checked\":" + std::to_string(report.checked);
  out += ",\"signature\":";
  out += report.signature_checked ? "true" : "false";
  out += ",\"notes\":";
  detail::json_list(&out, report.notes);
  return out + "}\n";
}

} // namespace mc::converter
