// SPDX-License-Identifier: GPL-3.0-or-later

#include "modapi/mod_package.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "json.hpp"
#include "modapi/zip_archive.h"
#include "pluginapi/plugin.h"

#ifndef PLUSMC_VERSION_STRING
#define PLUSMC_VERSION_STRING "0.1.0"
#endif

namespace mc::modapi {
namespace {

namespace fs = std::filesystem;
using detail::ZipArchive;
using detail::ZipEntry;

constexpr uint64_t kMaxPackageBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaxManifestBytes = 256 * 1024;
constexpr uint64_t kMaxClientAssetBytes = 64 * 1024 * 1024;
constexpr uint64_t kMaxPublicKeyBytes = 1024 * 1024;
constexpr std::size_t kMaxClientAssets = 1024;

std::string lower(std::string value) {
  for (char& c : value)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool starts_with(const std::string& text, const std::string& prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

std::string sha256_hex(const uint8_t* data, std::size_t size) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr)
    return {};
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  const void* input = size == 0 ? static_cast<const void*>("") : static_cast<const void*>(data);
  const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, input, size) == 1 &&
                  EVP_DigestFinal_ex(ctx, digest, &length) == 1;
  EVP_MD_CTX_free(ctx);
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

std::string sha256_hex(const std::vector<uint8_t>& bytes) {
  return sha256_hex(bytes.data(), bytes.size());
}

bool read_bytes_entry(const ZipArchive& zip, const ZipEntry& entry, std::vector<uint8_t>* out,
                      std::string* error) {
  if (entry.uncompressed_size > kMaxClientAssetBytes) {
    *error = "entry is too large: " + entry.name;
    return false;
  }
  return zip.read(entry, out, error);
}

bool safe_manifest_string(const std::string& value, std::string* error, const char* field) {
  if (value.empty() || value.size() > 256) {
    *error = std::string(field) + " is empty or too long";
    return false;
  }
  for (const unsigned char c : value) {
    if (c < 0x20 || c == 0x7f) {
      *error = std::string(field) + " contains a control character";
      return false;
    }
  }
  return true;
}

bool get_string(const pj::J& object, const char* key, std::string* out, std::string* error,
                const char* field) {
  const pj::J* value = object.find(key);
  if (value == nullptr || !value->is_string()) {
    *error = std::string(field) + " must be a string";
    return false;
  }
  *out = value->as_string();
  return safe_manifest_string(*out, error, field);
}

bool get_object(const pj::J& object, const char* key, const pj::J** out, std::string* error,
                const char* field) {
  const pj::J* value = object.find(key);
  if (value == nullptr || !value->is_object()) {
    *error = std::string(field) + " must be an object";
    return false;
  }
  *out = value;
  return true;
}

bool get_size(const pj::J& object, const char* key, uint64_t* out, std::string* error,
              const char* field) {
  const pj::J* value = object.find(key);
  if (value == nullptr || !value->is_number()) {
    *error = std::string(field) + " must be a number";
    return false;
  }
  const double number = value->as_number();
  if (number < 0 || number > static_cast<double>(std::numeric_limits<uint64_t>::max()) ||
      std::floor(number) != number) {
    *error = std::string(field) + " is out of range or not an integer";
    return false;
  }
  *out = static_cast<uint64_t>(number);
  return true;
}

struct Version {
  std::vector<uint64_t> parts;

  bool operator<(const Version& other) const {
    const std::size_t count = std::max(parts.size(), other.parts.size());
    for (std::size_t i = 0; i < count; ++i) {
      const uint64_t left = i < parts.size() ? parts[i] : 0;
      const uint64_t right = i < other.parts.size() ? other.parts[i] : 0;
      if (left != right)
        return left < right;
    }
    return false;
  }

  bool operator==(const Version& other) const {
    return !(*this < other) && !(other < *this);
  }
};

bool parse_version(const std::string& text, Version* out, std::string* error) {
  if (text.empty()) {
    *error = "version is empty";
    return false;
  }
  std::string value = text;
  if (value.front() == 'v' || value.front() == 'V')
    value.erase(value.begin());
  out->parts.clear();
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t dot = value.find('.', start);
    const std::string part =
        value.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (part.empty() || part.size() > 9 ||
        !std::all_of(part.begin(), part.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
      *error = "invalid version '" + text + "'";
      return false;
    }
    out->parts.push_back(static_cast<uint64_t>(std::stoull(part)));
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  if (out->parts.empty()) {
    *error = "invalid version '" + text + "'";
    return false;
  }
  while (out->parts.size() < 4)
    out->parts.push_back(0);
  return true;
}

bool version_in_range(const std::string& expression, const std::string& version,
                      std::string* error) {
  Version actual;
  if (!parse_version(version, &actual, error))
    return false;
  std::string normalized = expression;
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::istringstream stream(normalized);
  std::string token;
  bool any = false;
  while (stream >> token) {
    any = true;
    std::string op = "=";
    std::string number = token;
    if (starts_with(token, ">=") || starts_with(token, "<=") || starts_with(token, "==") ||
        starts_with(token, "!=")) {
      op = token.substr(0, 2);
      number = token.substr(2);
    } else if (starts_with(token, ">") || starts_with(token, "<") || starts_with(token, "=")) {
      op = token.substr(0, 1);
      number = token.substr(1);
    }
    Version expected;
    std::string why;
    if (!parse_version(number, &expected, &why)) {
      *error = "bad range '" + expression + "': " + why;
      return false;
    }
    const bool less = actual < expected;
    const bool equal = actual == expected;
    bool matches = false;
    if (op == ">=")
      matches = !less;
    else if (op == ">")
      matches = !less && !equal;
    else if (op == "<=")
      matches = less || equal;
    else if (op == "<")
      matches = less;
    else if (op == "=" || op == "==")
      matches = equal;
    else if (op == "!=")
      matches = !equal;
    if (!matches) {
      *error = "engine/API version " + version + " is outside '" + expression + "'";
      return false;
    }
  }
  if (!any) {
    *error = "version range is empty";
    return false;
  }
  return true;
}

bool target_parts(const std::string& target, std::string* os, std::string* arch) {
  const std::size_t dash = target.find('-');
  if (dash == std::string::npos) {
    *os = target;
    arch->clear();
    return false;
  }
  *os = target.substr(0, dash);
  *arch = target.substr(dash + 1);
  return (*os == "macos" || *os == "linux" || *os == "windows") &&
         (*arch == "x86_64" || *arch == "aarch64");
}

std::string current_host() {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "macos-aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "macos-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__) || defined(__arm64__)
  return "linux-aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "linux-x86_64";
#endif
#elif defined(_WIN32)
#if defined(_M_ARM64) || defined(__aarch64__)
  return "windows-aarch64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "windows-x86_64";
#endif
#endif
  return "unsupported";
}

uint16_t rd16(const std::vector<uint8_t>& bytes, std::size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(bytes[offset]) |
                               (static_cast<uint16_t>(bytes[offset + 1]) << 8));
}

uint32_t rd32(const std::vector<uint8_t>& bytes, std::size_t offset) {
  return static_cast<uint32_t>(static_cast<uint32_t>(bytes[offset]) |
                               (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                               (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                               (static_cast<uint32_t>(bytes[offset + 3]) << 24));
}

bool binary_format(const std::vector<uint8_t>& bytes, const std::string& target,
                   std::string* format, std::string* error) {
  std::string os;
  std::string arch;
  if (!target_parts(target, &os, &arch)) {
    *error = "unknown target '" + target + "'";
    return false;
  }
  if (os == "macos") {
    if (bytes.size() < 32 || bytes[0] != 0xcf || bytes[1] != 0xfa || bytes[2] != 0xed ||
        bytes[3] != 0xfe) {
      *error = "module is not a 64-bit little-endian Mach-O binary";
      return false;
    }
    const uint32_t cpu = rd32(bytes, 4);
    const uint32_t expected = arch == "aarch64" ? 0x0100000cu : 0x01000007u;
    if (cpu != expected) {
      *error = "Mach-O architecture does not match " + target;
      return false;
    }
    *format = std::string("Mach-O 64-bit ") + (arch == "aarch64" ? "arm64" : "x86-64");
    return true;
  }
  if (os == "linux") {
    if (bytes.size() < 64 || bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
      *error = "module is not an ELF binary";
      return false;
    }
    if (bytes[4] != 2 || bytes[5] != 1) {
      *error = "module is not a 64-bit little-endian ELF binary";
      return false;
    }
    const uint16_t machine = rd16(bytes, 18);
    const uint16_t expected = arch == "aarch64" ? 183u : 62u;
    if (machine != expected) {
      *error = "ELF architecture does not match " + target;
      return false;
    }
    *format = std::string("ELF 64-bit ") + (arch == "aarch64" ? "aarch64" : "x86-64");
    return true;
  }
  if (bytes.size() < 64 || bytes[0] != 'M' || bytes[1] != 'Z') {
    *error = "module is not a PE binary";
    return false;
  }
  const uint32_t pe_offset = rd32(bytes, 0x3c);
  if (static_cast<uint64_t>(pe_offset) + 24 > bytes.size() ||
      rd32(bytes, pe_offset) != 0x00004550u) {
    *error = "module has no PE header";
    return false;
  }
  const uint16_t machine = rd16(bytes, pe_offset + 4);
  const uint16_t expected = arch == "aarch64" ? 0xaa64u : 0x8664u;
  if (machine != expected) {
    *error = "PE architecture does not match " + target;
    return false;
  }
  if (rd16(bytes, pe_offset + 24) != 0x20bu) {
    *error = "module is not a PE32+ binary";
    return false;
  }
  *format = std::string("PE32+ ") + (arch == "aarch64" ? "arm64" : "x86-64");
  return true;
}

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

bool hex_decode(const std::string& text, std::vector<uint8_t>* out) {
  if (text.size() % 2 != 0)
    return false;
  out->clear();
  out->reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    const int high = digit(text[i]);
    const int low = digit(text[i + 1]);
    if (high < 0 || low < 0)
      return false;
    out->push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return true;
}

bool load_public_key(const fs::path& path, PkeyPtr* out, std::string* error) {
  std::error_code size_error;
  const uint64_t file_size = fs::file_size(path, size_error);
  if (size_error || file_size > kMaxPublicKeyBytes) {
    *error = "public key is missing or too large: " + path.string();
    return false;
  }
  std::vector<uint8_t> bytes;
  if (!read_file(path, &bytes, error))
    return false;
  EVP_PKEY* key = nullptr;
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  if (starts_with(text, "-----BEGIN")) {
    BIO* bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
    if (bio == nullptr) {
      *error = "cannot read public key " + path.string();
      return false;
    }
    key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
  } else {
    std::vector<uint8_t> raw;
    std::string value = text;
    if (starts_with(value, "ed25519:"))
      value = value.substr(8);
    if (value.size() == 64 && hex_decode(value, &raw)) {
      // decoded below
    } else if (value.size() == 32) {
      raw.assign(value.begin(), value.end());
    } else if (bytes.size() == 32) {
      raw = std::move(bytes);
    } else {
      *error = "public key must be PEM, 32 raw bytes, or 64 hex characters";
      return false;
    }
    key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw.data(), raw.size());
  }
  if (key == nullptr) {
    *error = "cannot parse public key " + path.string();
    return false;
  }
  PkeyPtr owned(key, &EVP_PKEY_free);
  if (EVP_PKEY_base_id(owned.get()) != EVP_PKEY_ED25519) {
    *error = "public key is not Ed25519: " + path.string();
    return false;
  }
  *out = std::move(owned);
  return true;
}

bool verify_one(EVP_PKEY* key, const std::vector<uint8_t>& signature,
                const std::vector<uint8_t>& message) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr)
    return false;
  const bool ok = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
                  EVP_DigestVerify(ctx, signature.data(), signature.size(), message.data(),
                                   message.size()) == 1;
  EVP_MD_CTX_free(ctx);
  return ok;
}

std::string join_targets(const std::vector<std::string>& targets) {
  std::string out;
  for (const std::string& target : targets) {
    if (!out.empty())
      out += ", ";
    out += target;
  }
  return out;
}

} // namespace

std::string current_host_target() {
  return current_host();
}

VerifyReport verify_package(const fs::path& path, const VerifyOptions& options) {
  VerifyReport report;
  report.package = path.string();
  report.status = "refused";
  report.host = options.host_target.empty() ? current_host() : options.host_target;
  report.engine = options.engine_version.empty() ? PLUSMC_VERSION_STRING : options.engine_version;
  report.api =
      options.abi_version.empty() ? std::to_string(PMC_PLUGIN_ABI_VERSION) : options.abi_version;
  const auto refuse = [&](std::string reason) {
    report.reason = std::move(reason);
    report.ready = false;
    report.status = "refused";
    return report;
  };

  try {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
      return refuse("package is not a regular file");
    const uint64_t package_size = fs::file_size(path, ec);
    if (ec || package_size > kMaxPackageBytes)
      return refuse("package is too large");
    std::vector<uint8_t> archive_bytes;
    std::string io_error;
    if (!read_file(path, &archive_bytes, &io_error))
      return refuse(io_error);
    report.archive_sha256 = sha256_hex(archive_bytes);
    if (report.archive_sha256.empty())
      return refuse("cannot hash package");

    if (report.host == "unsupported")
      return refuse("this build does not have a supported host target");

    ZipArchive zip;
    std::string zip_error;
    if (!zip.open(path, &zip_error))
      return refuse(zip_error);

    // This tool never extracts a client entry, but the package format still
    // forbids native client code. Rejecting it here keeps a package that the
    // launcher would later see from carrying an undeclared executable.
    for (const ZipEntry& entry : zip.entries()) {
      if (!starts_with(entry.name, "client/"))
        continue;
      const std::string name = lower(entry.name);
      for (const char* suffix : {".so", ".dylib", ".dll", ".exe", ".com"}) {
        if (ends_with(name, suffix))
          return refuse("native client entry is not allowed: " + entry.name);
      }
    }

    const ZipEntry* manifest_entry = zip.find("mod.json");
    if (manifest_entry == nullptr)
      return refuse("package has no mod.json");
    if (manifest_entry->uncompressed_size > kMaxManifestBytes)
      return refuse("mod.json is too large");
    std::vector<uint8_t> manifest_bytes;
    if (!read_bytes_entry(zip, *manifest_entry, &manifest_bytes, &zip_error))
      return refuse(zip_error);

    const ZipEntry* signature_entry = zip.find("mod.sig");
    if (signature_entry != nullptr) {
      if (signature_entry->uncompressed_size != 64)
        return refuse("mod.sig must be exactly 64 bytes");
      std::vector<uint8_t> signature;
      if (!read_bytes_entry(zip, *signature_entry, &signature, &zip_error))
        return refuse(zip_error);
      std::vector<PkeyPtr> keys;
      for (const fs::path& key_path : options.trusted_keys) {
        PkeyPtr key(nullptr, &EVP_PKEY_free);
        std::string key_error;
        if (!load_public_key(key_path, &key, &key_error)) {
          report.notes.push_back("ignored key: " + key_error);
          continue;
        }
        keys.push_back(std::move(key));
      }
      if (keys.empty())
        return refuse("package is signed, but no trusted public key was supplied");
      bool verified = false;
      for (const PkeyPtr& key : keys) {
        if (verify_one(key.get(), signature, manifest_bytes)) {
          verified = true;
          break;
        }
      }
      if (!verified)
        return refuse("Ed25519 signature does not match mod.json");
      report.signature = "signed";
    } else {
      if (!options.allow_unsigned)
        return refuse("package is unsigned; use --allow-unsigned only for local development");
      report.signature = "unsigned";
    }

    pj::J manifest;
    try {
      manifest = pj::J::parse(std::string(manifest_bytes.begin(), manifest_bytes.end()));
    } catch (const std::exception& e) {
      return refuse(std::string("invalid mod.json: ") + e.what());
    }
    if (!manifest.is_object())
      return refuse("mod.json must be an object");

    const pj::J* schema = manifest.find("schema");
    if (schema == nullptr || !schema->is_number() || schema->as_number() != 1.0)
      return refuse("unsupported mod.json schema (expected 1)");

    std::string validation_error;
    if (!get_string(manifest, "id", &report.id, &validation_error, "id") ||
        !get_string(manifest, "version", &report.version, &validation_error, "version"))
      return refuse(validation_error);
    if (!get_string(manifest, "name", &report.name, &validation_error, "name"))
      return refuse(validation_error);

    const pj::J* server = nullptr;
    if (!get_object(manifest, "server", &server, &validation_error, "server"))
      return refuse(validation_error);
    std::string api_range;
    std::string engine_range;
    if (!get_string(*server, "api", &api_range, &validation_error, "server.api") ||
        !get_string(*server, "engine", &engine_range, &validation_error, "server.engine"))
      return refuse(validation_error);
    if (!version_in_range(api_range, report.api, &validation_error))
      return refuse("ABI check failed: " + validation_error);
    if (!version_in_range(engine_range, report.engine, &validation_error))
      return refuse("engine check failed: " + validation_error);

    const pj::J* targets = nullptr;
    if (!get_object(*server, "targets", &targets, &validation_error, "server.targets"))
      return refuse(validation_error);
    const auto& target_items = targets->object_items();
    if (target_items.empty())
      return refuse("server.targets is empty");

    std::vector<std::string> available;
    const pj::J* selected = nullptr;
    for (const auto& [target, value] : target_items) {
      available.push_back(target);
      if (!value.is_object())
        return refuse("server.targets." + target + " must be an object");
      if (target == report.host)
        selected = &value;
    }
    if (selected == nullptr)
      return refuse("no module for host " + report.host + " (declared: " + join_targets(available) +
                    ")");

    // Verify every declared target, not only the one this host will load. A
    // package with a corrupt Windows module should be refused here, before it
    // is copied to a Windows machine.
    for (const auto& [target, value] : target_items) {
      std::string module_path;
      std::string module_hash;
      uint64_t module_size = 0;
      const std::string field = "server.targets." + target;
      if (!get_string(value, "module", &module_path, &validation_error,
                      (field + ".module").c_str()) ||
          !get_string(value, "sha256", &module_hash, &validation_error,
                      (field + ".sha256").c_str()) ||
          !get_size(value, "size", &module_size, &validation_error, (field + ".size").c_str()))
        return refuse(validation_error);
      if (!detail::safe_archive_path(module_path, &validation_error))
        return refuse(validation_error);
      const ZipEntry* entry = zip.find(module_path);
      if (entry == nullptr)
        return refuse("missing module " + module_path);
      if (entry->uncompressed_size != module_size)
        return refuse("size mismatch for " + module_path);
      std::vector<uint8_t> module_bytes;
      if (!read_bytes_entry(zip, *entry, &module_bytes, &zip_error))
        return refuse(zip_error);
      const std::string actual_hash = sha256_hex(module_bytes);
      if (actual_hash != lower(module_hash))
        return refuse("SHA-256 mismatch for " + module_path);
      std::string format;
      if (!binary_format(module_bytes, target, &format, &validation_error))
        return refuse(target + ": " + validation_error);
      if (target == report.host) {
        report.module.target = target;
        report.module.path = module_path;
        report.module.sha256 = actual_hash;
        report.module.format = format;
        report.module.size = module_size;
      }
    }
    if (report.module.path.empty())
      return refuse("selected module was not verified");
    if (zip.find("+mc-plugin.toml") == nullptr)
      return refuse("package has no +mc-plugin.toml");

    if (const pj::J* client = manifest.find("client"); client != nullptr) {
      if (!client->is_object())
        return refuse("client must be an object");
      std::string mode;
      if (const pj::J* mode_value = client->find("mode"); mode_value != nullptr) {
        if (!mode_value->is_string())
          return refuse("client.mode must be a string");
        mode = mode_value->as_string();
        if (mode != "none" && mode != "optional" && mode != "required")
          return refuse("client.mode must be none, optional or required");
      }
      if (const pj::J* module = client->find("module"); module != nullptr) {
        if (!module->is_object())
          return refuse("client.module must be an object");
        std::string client_path;
        std::string client_hash;
        uint64_t client_size = 0;
        if (!get_string(*module, "path", &client_path, &validation_error, "client.module.path") ||
            !get_string(*module, "sha256", &client_hash, &validation_error,
                        "client.module.sha256") ||
            !get_size(*module, "size", &client_size, &validation_error, "client.module.size"))
          return refuse(validation_error);
        if (!starts_with(client_path, "client/") || client_path.size() < 5 ||
            client_path.compare(client_path.size() - 5, 5, ".wasm") != 0)
          return refuse("client.module.path must be a .wasm file under client/");
        const ZipEntry* entry = zip.find(client_path);
        if (entry == nullptr)
          return refuse("missing client module " + client_path);
        if (entry->uncompressed_size != client_size)
          return refuse("size mismatch for " + client_path);
        std::vector<uint8_t> bytes;
        if (!read_bytes_entry(zip, *entry, &bytes, &zip_error))
          return refuse(zip_error);
        if (sha256_hex(bytes) != lower(client_hash))
          return refuse("SHA-256 mismatch for " + client_path);
      } else if (mode == "required" || mode == "optional") {
        return refuse("client.mode is " + mode + " but no client module is declared");
      }
      if (const pj::J* assets = client->find("assets"); assets != nullptr) {
        if (!assets->is_array())
          return refuse("client.assets must be an array");
        if (assets->array_items().size() > kMaxClientAssets)
          return refuse("client.assets has too many entries");
        for (const pj::J& asset : assets->array_items()) {
          if (!asset.is_object())
            return refuse("client.assets entries must be objects");
          std::string asset_path;
          std::string asset_hash;
          uint64_t asset_size = 0;
          if (!get_string(asset, "path", &asset_path, &validation_error, "client.assets.path") ||
              !get_string(asset, "sha256", &asset_hash, &validation_error,
                          "client.assets.sha256") ||
              !get_size(asset, "size", &asset_size, &validation_error, "client.assets.size"))
            return refuse(validation_error);
          if (!starts_with(asset_path, "assets/"))
            return refuse("client asset '" + asset_path + "' must be under assets/");
          const ZipEntry* entry = zip.find(asset_path);
          if (entry == nullptr)
            return refuse("missing client asset " + asset_path);
          if (entry->uncompressed_size != asset_size)
            return refuse("size mismatch for " + asset_path);
          std::vector<uint8_t> bytes;
          if (!read_bytes_entry(zip, *entry, &bytes, &zip_error))
            return refuse(zip_error);
          if (sha256_hex(bytes) != lower(asset_hash))
            return refuse("SHA-256 mismatch for " + asset_path);
        }
      }
    }

    if (const pj::J* depends = manifest.find("depends"); depends != nullptr) {
      if (!depends->is_array())
        return refuse("depends must be an array");
      for (const pj::J& dependency : depends->array_items()) {
        std::string dependency_id;
        std::string dependency_range;
        if (!dependency.is_object() ||
            !get_string(dependency, "id", &dependency_id, &validation_error, "depends.id") ||
            !get_string(dependency, "range", &dependency_range, &validation_error, "depends.range"))
          return refuse(validation_error.empty() ? "each dependency must be an object"
                                                 : validation_error);
      }
    }

    report.status = "ready";
    report.ready = true;
    return report;
  } catch (const std::exception& e) {
    return refuse(std::string("verification failed: ") + e.what());
  }
}

ExtractReport extract_package(const fs::path& path, const fs::path& destination,
                              const ExtractOptions& options) {
  ExtractReport report;
  report.destination = destination.string();
  report.verification = verify_package(path, options.verify);
  if (!report.verification.ready) {
    report.error = report.verification.reason;
    return report;
  }
  if (destination.empty()) {
    report.error = "destination is empty";
    return report;
  }

  try {
    std::error_code ec;
    if (fs::exists(destination, ec) && !options.replace) {
      report.error = "destination already exists: " + destination.string();
      return report;
    }
    const fs::path temporary = destination.string() + ".extract-tmp";
    fs::remove_all(temporary, ec);
    if (!fs::create_directories(temporary, ec) && ec) {
      report.error = "cannot create extraction directory: " + ec.message();
      return report;
    }

    ZipArchive zip;
    std::string zip_error;
    if (!zip.open(path, &zip_error)) {
      report.error = zip_error;
      fs::remove_all(temporary, ec);
      return report;
    }
    for (const ZipEntry& entry : zip.entries()) {
      const fs::path target = temporary / fs::path(entry.name);
      if (!entry.name.empty() && entry.name.back() == '/') {
        if (!fs::create_directories(target, ec) && ec) {
          report.error = "cannot create " + target.string() + ": " + ec.message();
          fs::remove_all(temporary, ec);
          return report;
        }
        continue;
      }
      std::vector<uint8_t> bytes;
      if (!zip.read(entry, &bytes, &zip_error)) {
        report.error = zip_error;
        fs::remove_all(temporary, ec);
        return report;
      }
      if (!target.parent_path().empty() && !fs::create_directories(target.parent_path(), ec) &&
          ec) {
        report.error = "cannot create " + target.parent_path().string() + ": " + ec.message();
        fs::remove_all(temporary, ec);
        return report;
      }
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      if (!out) {
        report.error = "cannot write " + target.string();
        fs::remove_all(temporary, ec);
        return report;
      }
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
      if (!out) {
        report.error = "write failed: " + target.string();
        fs::remove_all(temporary, ec);
        return report;
      }
    }

    if (fs::exists(destination, ec)) {
      fs::remove_all(destination, ec);
      if (ec) {
        report.error = "cannot replace " + destination.string() + ": " + ec.message();
        fs::remove_all(temporary, ec);
        return report;
      }
    }
    fs::rename(temporary, destination, ec);
    if (ec) {
      report.error = "cannot install extracted package: " + ec.message();
      fs::remove_all(temporary, ec);
      return report;
    }
    report.extracted = true;
    return report;
  } catch (const std::exception& e) {
    report.error = std::string("extraction failed: ") + e.what();
    return report;
  }
}

} // namespace mc::modapi
