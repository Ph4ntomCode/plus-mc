// SPDX-License-Identifier: GPL-3.0-or-later
//
// `plusmc-mod` is the server-side preflight for `.pmcmod` packages. It does
// not load a module: it checks the archive, signature, hashes, version ranges,
// host target and native binary header, then prints the decision.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include "modapi/mod_pack.h"
#include "modapi/mod_package.h"

namespace {

namespace fs = std::filesystem;

void usage() {
  std::cout << "usage:\n"
            << "  plusmc-mod verify <package.pmcmod> [--key <public.pem>]...\n"
            << "                   [--host <target>] [--engine <version>] [--abi <version>]\n"
            << "                   [--allow-unsigned]\n"
            << "  plusmc-mod pack --dir <staging> --out <package.pmcmod> [--key <private.pem>]\n"
            << "  plusmc-mod extract <package.pmcmod> --out <dir> [--key <public.pem>]...\n"
            << "                   [--allow-unsigned] [--replace]\n"
            << "  plusmc-mod host\n"
            << "  plusmc-mod keygen --private <key.pem> --public <key.pem>\n"
            << "  plusmc-mod sign --key <private.pem> --in <mod.json> --out <mod.sig>\n";
}

int fail(const std::string& message) {
  std::cerr << "plusmc-mod: " << message << "\n";
  return 2;
}

bool read_file(const fs::path& path, std::string* out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot read " + path.string();
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool write_file(const fs::path& path, const void* data, std::size_t size, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot write " + path.string();
    return false;
  }
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!out) {
    *error = "write failed: " + path.string();
    return false;
  }
  return true;
}

int keygen(const fs::path& private_path, const fs::path& public_path) {
  EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
  if (context == nullptr)
    return fail("OpenSSL is unavailable");
  EVP_PKEY* key = nullptr;
  const bool generated = EVP_PKEY_keygen_init(context) == 1 && EVP_PKEY_keygen(context, &key) == 1;
  EVP_PKEY_CTX_free(context);
  if (!generated || key == nullptr)
    return fail("cannot generate an Ed25519 key");

  bool ok = true;
  FILE* private_file = std::fopen(private_path.string().c_str(), "wb");
  if (private_file == nullptr) {
    ok = false;
  } else {
    ok = PEM_write_PrivateKey(private_file, key, nullptr, nullptr, 0, nullptr, nullptr) == 1;
    std::fclose(private_file);
  }
  FILE* public_file = ok ? std::fopen(public_path.string().c_str(), "wb") : nullptr;
  if (public_file == nullptr) {
    ok = false;
  } else {
    ok = PEM_write_PUBKEY(public_file, key) == 1;
    std::fclose(public_file);
  }
  EVP_PKEY_free(key);
  if (!ok)
    return fail("cannot write the key pair");
  return 0;
}

int sign(const fs::path& key_path, const fs::path& input_path, const fs::path& output_path) {
  std::string message;
  std::string error;
  if (!read_file(input_path, &message, &error))
    return fail(error);

  FILE* key_file = std::fopen(key_path.string().c_str(), "rb");
  if (key_file == nullptr)
    return fail("cannot read private key " + key_path.string());
  EVP_PKEY* key = PEM_read_PrivateKey(key_file, nullptr, nullptr, nullptr);
  std::fclose(key_file);
  if (key == nullptr)
    return fail("cannot parse private key " + key_path.string());
  if (EVP_PKEY_base_id(key) != EVP_PKEY_ED25519) {
    EVP_PKEY_free(key);
    return fail("private key is not Ed25519");
  }

  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    EVP_PKEY_free(key);
    return fail("OpenSSL is unavailable");
  }
  std::size_t signature_size = 0;
  bool ok =
      EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key) == 1 &&
      EVP_DigestSign(context, nullptr, &signature_size,
                     reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
  std::vector<uint8_t> signature(signature_size);
  if (ok)
    ok =
        EVP_DigestSign(context, signature.data(), &signature_size,
                       reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
  EVP_MD_CTX_free(context);
  EVP_PKEY_free(key);
  if (!ok)
    return fail("cannot sign " + input_path.string());
  signature.resize(signature_size);
  if (!write_file(output_path, signature.data(), signature.size(), &error))
    return fail(error);
  return 0;
}

int verify(const std::string& package, const mc::modapi::VerifyOptions& options) {
  const mc::modapi::VerifyReport report = mc::modapi::verify_package(package, options);
  std::cout << "status: " << report.status << "\n";
  std::cout << "package: " << report.package << "\n";
  std::cout << "archive sha256: " << report.archive_sha256 << "\n";
  std::cout << "id: " << report.id << "\n";
  std::cout << "name: " << report.name << "\n";
  std::cout << "version: " << report.version << "\n";
  std::cout << "host: " << report.host << "\n";
  std::cout << "engine: " << report.engine << "\n";
  std::cout << "abi: " << report.api << "\n";
  std::cout << "signature: " << report.signature << "\n";
  if (!report.module.target.empty()) {
    std::cout << "target: " << report.module.target << "\n";
    std::cout << "module: " << report.module.path << "\n";
    std::cout << "module format: " << report.module.format << "\n";
    std::cout << "module sha256: " << report.module.sha256 << "\n";
    std::cout << "module size: " << report.module.size << "\n";
  }
  if (!report.reason.empty())
    std::cout << "reason: " << report.reason << "\n";
  for (const std::string& note : report.notes)
    std::cout << "note: " << note << "\n";
  return report.ready ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string command = argv[1];
  if (command == "host") {
    std::cout << mc::modapi::current_host_target() << "\n";
    return 0;
  }
  if (command == "keygen") {
    fs::path private_path;
    fs::path public_path;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--private" && i + 1 < argc)
        private_path = argv[++i];
      else if (arg == "--public" && i + 1 < argc)
        public_path = argv[++i];
      else
        return fail("unknown keygen argument " + arg);
    }
    if (private_path.empty() || public_path.empty())
      return fail("keygen needs --private and --public");
    return keygen(private_path, public_path);
  }
  if (command == "sign") {
    fs::path key_path;
    fs::path input_path;
    fs::path output_path;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--key" && i + 1 < argc)
        key_path = argv[++i];
      else if (arg == "--in" && i + 1 < argc)
        input_path = argv[++i];
      else if (arg == "--out" && i + 1 < argc)
        output_path = argv[++i];
      else
        return fail("unknown sign argument " + arg);
    }
    if (key_path.empty() || input_path.empty() || output_path.empty())
      return fail("sign needs --key, --in and --out");
    return sign(key_path, input_path, output_path);
  }
  if (command == "pack") {
    fs::path source;
    fs::path output;
    fs::path key;
    for (int i = 2; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--dir" && i + 1 < argc)
        source = argv[++i];
      else if (arg == "--out" && i + 1 < argc)
        output = argv[++i];
      else if (arg == "--key" && i + 1 < argc)
        key = argv[++i];
      else
        return fail("unknown pack argument " + arg);
    }
    if (source.empty() || output.empty())
      return fail("pack needs --dir and --out");
    mc::modapi::PackOptions options;
    options.source_dir = source;
    options.output_path = output;
    options.signing_key = key;
    const mc::modapi::PackReport report = mc::modapi::pack_directory(options);
    if (!report.ok)
      return fail(report.error);
    std::cout << "packed: " << output.string() << "\n";
    std::cout << "archive sha256: " << report.archive_sha256 << "\n";
    return 0;
  }
  if (command == "extract") {
    if (argc < 3) {
      usage();
      return 2;
    }
    const std::string package = argv[2];
    fs::path destination;
    bool replace = false;
    mc::modapi::ExtractOptions options;
    for (int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--out" && i + 1 < argc)
        destination = argv[++i];
      else if (arg == "--key" && i + 1 < argc)
        options.verify.trusted_keys.emplace_back(argv[++i]);
      else if (arg == "--host" && i + 1 < argc)
        options.verify.host_target = argv[++i];
      else if (arg == "--engine" && i + 1 < argc)
        options.verify.engine_version = argv[++i];
      else if (arg == "--abi" && i + 1 < argc)
        options.verify.abi_version = argv[++i];
      else if (arg == "--allow-unsigned")
        options.verify.allow_unsigned = true;
      else if (arg == "--replace")
        replace = true;
      else
        return fail("unknown extract argument " + arg);
    }
    if (destination.empty())
      return fail("extract needs --out");
    options.replace = replace;
    const mc::modapi::ExtractReport report =
        mc::modapi::extract_package(package, destination, options);
    if (!report.extracted) {
      std::cout << "status: refused\n";
      std::cout << "reason: " << report.error << "\n";
      return 1;
    }
    std::cout << "status: extracted\n";
    std::cout << "package: " << report.verification.package << "\n";
    std::cout << "destination: " << report.destination << "\n";
    std::cout << "archive sha256: " << report.verification.archive_sha256 << "\n";
    std::cout << "id: " << report.verification.id << "\n";
    std::cout << "version: " << report.verification.version << "\n";
    return 0;
  }
  if (command == "verify") {
    if (argc < 3) {
      usage();
      return 2;
    }
    mc::modapi::VerifyOptions options;
    const std::string package = argv[2];
    for (int i = 3; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--key" && i + 1 < argc)
        options.trusted_keys.emplace_back(argv[++i]);
      else if (arg == "--host" && i + 1 < argc)
        options.host_target = argv[++i];
      else if (arg == "--engine" && i + 1 < argc)
        options.engine_version = argv[++i];
      else if (arg == "--abi" && i + 1 < argc)
        options.abi_version = argv[++i];
      else if (arg == "--allow-unsigned")
        options.allow_unsigned = true;
      else
        return fail("unknown verify argument " + arg);
    }
    return verify(package, options);
  }
  usage();
  return 2;
}
