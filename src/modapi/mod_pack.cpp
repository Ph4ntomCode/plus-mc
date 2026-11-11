// SPDX-License-Identifier: GPL-3.0-or-later

#include "modapi/mod_pack.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "json.hpp"
#include "modapi/zip_archive.h"
#include "modapi/zip_writer.h"

namespace mc::modapi {
namespace {

namespace fs = std::filesystem;
using detail::ZipWriteEntry;

constexpr uint64_t kMaxManifestBytes = 256 * 1024;
constexpr uint64_t kMaxFileBytes = 64 * 1024 * 1024;

std::string sha256_hex(const std::vector<uint8_t>& bytes) {
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr)
    return {};
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  const void* input = bytes.empty() ? static_cast<const void*>("") : bytes.data();
  const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                  EVP_DigestUpdate(ctx, input, bytes.size()) == 1 &&
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

bool read_file(const fs::path& path, std::vector<uint8_t>* out, uint64_t max_bytes,
               std::string* error) {
  std::error_code ec;
  const uint64_t size = fs::file_size(path, ec);
  if (ec || size > max_bytes) {
    *error = "file is missing or too large: " + path.string();
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

void append_escaped_string(const std::string& value, std::string* out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out->append("\\\"");
      break;
    case '\\':
      out->append("\\\\");
      break;
    case '\b':
      out->append("\\b");
      break;
    case '\f':
      out->append("\\f");
      break;
    case '\n':
      out->append("\\n");
      break;
    case '\r':
      out->append("\\r");
      break;
    case '\t':
      out->append("\\t");
      break;
    default:
      if (c < 0x20) {
        out->append("\\u00");
        out->push_back(kHex[c >> 4]);
        out->push_back(kHex[c & 0x0f]);
      } else {
        out->push_back(static_cast<char>(c));
      }
      break;
    }
  }
  out->push_back('"');
}

bool dump_json(const pj::J& value, std::string* out, std::string* error) {
  switch (value.type()) {
  case pj::J::Type::Null:
    out->append("null");
    return true;
  case pj::J::Type::Bool:
    out->append(value.as_bool() ? "true" : "false");
    return true;
  case pj::J::Type::String:
    append_escaped_string(value.as_string(), out);
    return true;
  case pj::J::Type::Number: {
    const double number = value.as_number();
    if (!std::isfinite(number)) {
      *error = "manifest contains a non-finite number";
      return false;
    }
    if (std::floor(number) == number && std::fabs(number) < 9.0e15) {
      out->append(std::to_string(static_cast<long long>(number)));
    } else {
      std::ostringstream stream;
      stream.imbue(std::locale::classic());
      stream << std::setprecision(17) << number;
      out->append(stream.str());
    }
    return true;
  }
  case pj::J::Type::Array:
    out->push_back('[');
    for (std::size_t i = 0; i < value.array_items().size(); ++i) {
      if (i != 0)
        out->push_back(',');
      if (!dump_json(value.array_items()[i], out, error))
        return false;
    }
    out->push_back(']');
    return true;
  case pj::J::Type::Object: {
    std::vector<std::pair<std::string, pj::J>> items = value.object_items();
    std::sort(items.begin(), items.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
    out->push_back('{');
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i != 0)
        out->push_back(',');
      append_escaped_string(items[i].first, out);
      out->push_back(':');
      if (!dump_json(items[i].second, out, error))
        return false;
    }
    out->push_back('}');
    return true;
  }
  }
  *error = "manifest contains an unknown JSON type";
  return false;
}

bool update_file_descriptor(pj::J& object, const fs::path& root, const char* path_field,
                            std::string* error) {
  const pj::J* path_value = object.find(path_field);
  if (path_value == nullptr || !path_value->is_string()) {
    *error = std::string(path_field) + " must be a string";
    return false;
  }
  const std::string relative = path_value->as_string();
  if (!detail::safe_archive_path(relative, error))
    return false;
  std::vector<uint8_t> bytes;
  if (!read_file(root / fs::path(relative), &bytes, kMaxFileBytes, error))
    return false;
  const std::string hash = sha256_hex(bytes);
  if (hash.empty()) {
    *error = "cannot hash " + relative;
    return false;
  }
  object.set("sha256", pj::J(hash));
  object.set("size", pj::J(static_cast<double>(bytes.size())));
  return true;
}

bool sign_manifest(const fs::path& key_path, const std::vector<uint8_t>& manifest,
                   std::vector<uint8_t>* signature, std::string* error) {
  FILE* key_file = std::fopen(key_path.string().c_str(), "rb");
  if (key_file == nullptr) {
    *error = "cannot read signing key " + key_path.string();
    return false;
  }
  EVP_PKEY* key = PEM_read_PrivateKey(key_file, nullptr, nullptr, nullptr);
  std::fclose(key_file);
  if (key == nullptr) {
    *error = "cannot parse signing key " + key_path.string();
    return false;
  }
  if (EVP_PKEY_base_id(key) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(key);
    *error = "signing key is not Ed25519";
    return false;
  }
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr) {
    EVP_PKEY_free(key);
    *error = "OpenSSL is unavailable";
    return false;
  }
  std::size_t length = 0;
  bool ok = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
            EVP_DigestSign(ctx, nullptr, &length, manifest.data(), manifest.size()) == 1;
  signature->assign(length, 0);
  if (ok)
    ok = EVP_DigestSign(ctx, signature->data(), &length, manifest.data(), manifest.size()) == 1;
  EVP_MD_CTX_free(ctx);
  EVP_PKEY_free(key);
  if (!ok) {
    *error = "cannot sign mod.json";
    return false;
  }
  signature->resize(length);
  return true;
}

bool path_is_inside(const fs::path& child, const fs::path& parent) {
  std::error_code ec;
  const fs::path child_abs = fs::weakly_canonical(child, ec);
  if (ec)
    return false;
  const fs::path parent_abs = fs::weakly_canonical(parent, ec);
  if (ec)
    return false;
  const fs::path relative = child_abs.lexically_relative(parent_abs);
  if (relative.empty())
    return false;
  return relative.begin() != relative.end() && *relative.begin() != "..";
}

} // namespace

PackReport pack_directory(const PackOptions& options) {
  PackReport report;
  const auto fail = [&](std::string why) {
    report.ok = false;
    report.error = std::move(why);
    return report;
  };
  try {
    std::error_code ec;
    if (!fs::is_directory(options.source_dir, ec))
      return fail("source is not a directory: " + options.source_dir.string());
    if (options.output_path.empty())
      return fail("output path is empty");
    if (path_is_inside(options.output_path, options.source_dir))
      return fail("output package must be outside the staging directory");

    std::vector<uint8_t> manifest_bytes;
    if (!read_file(options.source_dir / "mod.json", &manifest_bytes, kMaxManifestBytes,
                   &report.error))
      return fail(report.error);
    pj::J manifest = pj::J::parse(std::string(manifest_bytes.begin(), manifest_bytes.end()));
    if (!manifest.is_object())
      return fail("mod.json must be an object");

    pj::J* server = manifest.find("server");
    if (server == nullptr || !server->is_object())
      return fail("server must be an object");
    pj::J* targets = server->find("targets");
    if (targets == nullptr || !targets->is_object() || targets->object_items().empty())
      return fail("server.targets must be a non-empty object");
    for (auto& [target, value] : targets->object_items()) {
      if (!value.is_object())
        return fail("server.targets." + target + " must be an object");
      if (!update_file_descriptor(value, options.source_dir, "module", &report.error))
        return fail(report.error);
    }

    if (pj::J* client = manifest.find("client"); client != nullptr) {
      if (!client->is_object())
        return fail("client must be an object");
      if (pj::J* module = client->find("module"); module != nullptr) {
        if (!module->is_object())
          return fail("client.module must be an object");
        if (!update_file_descriptor(*module, options.source_dir, "path", &report.error))
          return fail(report.error);
      }
      if (pj::J* assets = client->find("assets"); assets != nullptr) {
        if (!assets->is_array())
          return fail("client.assets must be an array");
        for (pj::J& asset : assets->array_items()) {
          if (!asset.is_object())
            return fail("client.assets entries must be objects");
          if (!update_file_descriptor(asset, options.source_dir, "path", &report.error))
            return fail(report.error);
        }
      }
    }

    std::string canonical;
    if (!dump_json(manifest, &canonical, &report.error))
      return fail(report.error);
    std::vector<uint8_t> canonical_bytes(canonical.begin(), canonical.end());

    std::map<std::string, std::vector<uint8_t>> files;
    for (fs::recursive_directory_iterator
             it(options.source_dir, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec)) {
      if (ec)
        return fail("cannot walk staging directory: " + ec.message());
      const fs::path path = it->path();
      if (it->is_symlink(ec)) {
        return fail("staging directory contains a symlink: " + path.string());
      }
      if (!it->is_regular_file(ec))
        continue;
      const std::string relative = path.lexically_relative(options.source_dir).generic_string();
      if (relative == "mod.json" || relative == "mod.sig")
        continue;
      if (!detail::safe_archive_path(relative, &report.error))
        return fail(report.error);
      std::vector<uint8_t> bytes;
      if (!read_file(path, &bytes, kMaxFileBytes, &report.error))
        return fail(report.error);
      files.emplace(relative, std::move(bytes));
    }
    if (files.find("+mc-plugin.toml") == files.end())
      return fail("staging directory has no +mc-plugin.toml");

    std::vector<ZipWriteEntry> entries;
    entries.push_back({"mod.json", std::move(canonical_bytes)});
    if (!options.signing_key.empty()) {
      std::vector<uint8_t> signature;
      if (!sign_manifest(options.signing_key, entries.front().data, &signature, &report.error))
        return fail(report.error);
      entries.push_back({"mod.sig", std::move(signature)});
    }
    for (auto& [name, bytes] : files)
      entries.push_back({name, std::move(bytes)});

    const fs::path temporary = options.output_path.string() + ".tmp";
    fs::remove(temporary, ec);
    if (!detail::write_zip(temporary, std::move(entries), &report.error))
      return fail(report.error);
    fs::remove(options.output_path, ec);
    fs::rename(temporary, options.output_path, ec);
    if (ec) {
      fs::remove(temporary, ec);
      return fail("cannot install package: " + ec.message());
    }

    std::vector<uint8_t> archive;
    if (!read_file(options.output_path, &archive, 512ull * 1024 * 1024, &report.error))
      return fail(report.error);
    report.archive_sha256 = sha256_hex(archive);
    if (report.archive_sha256.empty())
      return fail("cannot hash package");
    report.ok = true;
    return report;
  } catch (const std::exception& e) {
    return fail(std::string("pack failed: ") + e.what());
  }
}

} // namespace mc::modapi
