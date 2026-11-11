// SPDX-License-Identifier: GPL-3.0-or-later
//
// Online-mode (premium) authentication primitives for the login state:
//
//   * ServerKeys — the server's 1024-bit RSA keypair (one per process, as
//     vanilla generates at startup). The public key, DER-encoded as X.509
//     SubjectPublicKeyInfo, goes in the Encryption Request packet; the private
//     key decrypts the client's PKCS#1 v1.5 padded shared secret + verify
//     token (docs/PHASE1.md "online mode" slice).
//
//   * mc_auth_hex_digest — the non-standard Minecraft SHA-1 hexdigest over
//     (server id || shared secret || DER public key): the digest bytes are
//     interpreted as a signed big-endian integer and printed in base 16,
//     prefixed with '-' when negative (Java's BigInteger.toString(16)).
//
//   * query_has_joined — Yggdrasil "hasJoined" GET. The default endpoint is
//     https://sessionserver.mojang.com/session/minecraft/hasJoined; the env
//     var PLUSMC_AUTH_URL overrides it (plain http:// URLs are allowed so
//     integration tests can stub the session server).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mc::server {

// One profile property from the hasJoined response (e.g. "textures").
struct ProfileProperty {
  std::string name;
  std::string value;
  bool has_signature = false;
  std::string signature;
};

// The authenticated identity a client gets in Login Success (online mode).
struct MojangProfile {
  std::string name;
  uint8_t uuid[16]{}; // raw bytes (dashes stripped)
  std::vector<ProfileProperty> properties;
};

// Server RSA keypair (1024-bit, generated on first use). Read-only after
// construction, so a shared instance is safe across session threads.
class ServerKeys {
public:
  static const ServerKeys& instance();

  // Public key in DER SubjectPublicKeyInfo form (what the client's key loader
  // and the auth digest expect).
  const std::vector<uint8_t>& public_key_der() const;
  // Size of an encrypted block in bytes (128 for RSA-1024).
  size_t key_size() const;

  // Decrypts one RSA PKCS#1 v1.5 padded block. Returns false on failure
  // (wrong key, malformed padding).
  bool decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>* out) const;

private:
  ServerKeys();
  ~ServerKeys();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Fills `out` with cryptographically-random bytes (OpenSSL RNG). Returns
// false when the RNG fails.
bool mc_random_bytes(uint8_t* out, size_t n);

// Minecraft-style signed-hex SHA-1 over (server_id || secret || pubkey DER).
std::string mc_auth_hex_digest(const std::string& server_id,
                               const std::vector<uint8_t>& shared_secret,
                               const std::vector<uint8_t>& public_key_der);

enum class AuthCheck {
  Ok,        // 200: the profile is returned
  NotJoined, // 204/404: no matching session (not a premium user / wrong hash)
  Error,     // network or server failure (unreachable, bad response, ...)
};

struct AuthResult {
  AuthCheck check = AuthCheck::Error;
  MojangProfile profile; // valid when check == Ok
  std::string error;     // human-readable detail (logged, not sent to client)
};

// Queries the Yggdrasil session server about `username` (blocking). `hash` is
// mc_auth_hex_digest(...). Never throws.
AuthResult query_has_joined(const std::string& username, const std::string& hash);

} // namespace mc::server
