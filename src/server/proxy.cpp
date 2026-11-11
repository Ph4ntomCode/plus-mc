// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/proxy.h"

#include <cstring>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "net/byte_buffer.h"

namespace mc::server {

namespace {

// A 32-char undashed UUID ("069a79f444e94726a5befca90e38aaf5") into 16 bytes.
bool parse_undashed_uuid(const std::string& text, uint8_t out[16]) {
  if (text.size() != 32)
    return false;
  for (size_t i = 0; i < 16; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    const int hi = nibble(text[i * 2]), lo = nibble(text[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

// The legacy properties field is a JSON array of objects, each
// `{"name":..,"value":..[,"signature":..]}` (BungeeCord writes it with GSON).
// Values are base64, so they carry no quotes or escapes; a tolerant scan for
// string key/value pairs is enough, and anything malformed degrades to "no
// properties" rather than failing the login — the same posture the hosting
// player-list reader takes (server/hosting.cpp).
void parse_properties_json(const std::string& json, std::vector<ProfileProperty>* out) {
  size_t pos = 0;
  while (true) {
    const size_t obj = json.find('{', pos);
    if (obj == std::string::npos)
      return;
    const size_t end = json.find('}', obj);
    if (end == std::string::npos)
      return;
    const std::string body = json.substr(obj, end - obj);
    pos = end + 1;

    ProfileProperty prop;
    size_t scan = 0;
    while (true) {
      const size_t key_open = body.find('"', scan);
      if (key_open == std::string::npos)
        break;
      const size_t key_close = body.find('"', key_open + 1);
      if (key_close == std::string::npos)
        break;
      const std::string key = body.substr(key_open + 1, key_close - key_open - 1);
      const size_t colon = body.find(':', key_close);
      if (colon == std::string::npos)
        break;
      const size_t value_start = body.find_first_not_of(" \t\r\n", colon + 1);
      if (value_start == std::string::npos)
        break;
      if (body[value_start] != '"') {
        // A non-string value (there is none in this schema) — skip the pair.
        scan = value_start + 1;
        continue;
      }
      const size_t value_close = body.find('"', value_start + 1);
      if (value_close == std::string::npos)
        break;
      const std::string value = body.substr(value_start + 1, value_close - value_start - 1);
      if (key == "name")
        prop.name = value;
      else if (key == "value")
        prop.value = value;
      else if (key == "signature") {
        prop.signature = value;
        prop.has_signature = true;
      }
      scan = value_close + 1;
    }
    if (!prop.name.empty() || !prop.value.empty())
      out->push_back(std::move(prop));
  }
}

} // namespace

bool has_legacy_forwarding(const std::string& handshake_address) {
  return handshake_address.find('\0') != std::string::npos;
}

ForwardedIdentity parse_legacy_forwarding(const std::string& handshake_address) {
  ForwardedIdentity out;
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t nul = handshake_address.find('\0', start);
    if (nul == std::string::npos) {
      parts.push_back(handshake_address.substr(start));
      break;
    }
    parts.push_back(handshake_address.substr(start, nul - start));
    start = nul + 1;
  }
  // BungeeCord always writes three fields, and a fourth — the profile — only
  // when the proxy actually has properties to forward.
  if (parts.size() != 3 && parts.size() != 4) {
    out.error =
        "legacy forwarding has " + std::to_string(parts.size()) + " field(s), expected 3 or 4";
    return out;
  }
  out.address = parts[1];
  if (!parse_undashed_uuid(parts[2], out.uuid)) {
    out.error = "legacy forwarding UUID is not 32 hex characters";
    return out;
  }
  if (parts.size() == 4 && !parts[3].empty())
    parse_properties_json(parts[3], &out.properties);
  out.ok = true;
  return out;
}

std::vector<uint8_t> velocity_request_payload() {
  return {kVelocityForwardingVersion};
}

ForwardedIdentity verify_velocity_forwarding(const std::string& secret,
                                             const std::vector<uint8_t>& data) {
  ForwardedIdentity out;
  constexpr size_t kSignatureBytes = 32; // HMAC-SHA256
  if (data.size() < kSignatureBytes) {
    out.error = "response is shorter than its signature";
    return out;
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  if (!HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
            data.data() + kSignatureBytes, data.size() - kSignatureBytes, digest, &digest_len) ||
      digest_len != kSignatureBytes) {
    out.error = "could not compute the expected signature";
    return out;
  }
  // Constant-time: the signature is a secret an attacker is trying to guess.
  if (CRYPTO_memcmp(digest, data.data(), kSignatureBytes) != 0) {
    out.error = "signature mismatch";
    return out;
  }

  std::vector<uint8_t> body(data.begin() + static_cast<ptrdiff_t>(kSignatureBytes), data.end());
  mc::net::Reader r(body);
  uint32_t version = 0;
  if (!r.varint(&version)) {
    out.error = "malformed payload (no version)";
    return out;
  }
  if (version > kVelocityForwardingVersion) {
    out.error = "forwarding version " + std::to_string(version) + " is newer than " +
                std::to_string(kVelocityForwardingVersion);
    return out;
  }
  // Version 1 is the baseline and carries the address; the field only exists
  // from 1 on, so a (hypothetical) version 0 has none.
  if (version >= 1) {
    if (!r.string(&out.address)) {
      out.error = "malformed payload (address)";
      return out;
    }
  } else {
    out.address = "<unknown>";
  }
  std::vector<uint8_t> uuid;
  if (!r.bytes(16, &uuid)) {
    out.error = "malformed payload (uuid)";
    return out;
  }
  std::memcpy(out.uuid, uuid.data(), 16);
  if (!r.string(&out.name)) {
    out.error = "malformed payload (username)";
    return out;
  }
  uint32_t count = 0;
  if (!r.varint(&count)) {
    out.error = "malformed payload (property count)";
    return out;
  }
  for (uint32_t i = 0; i < count; ++i) {
    ProfileProperty prop;
    uint8_t has_signature = 0;
    if (!r.string(&prop.name) || !r.string(&prop.value) || !r.u8(&has_signature)) {
      out.error = "malformed payload (property)";
      return out;
    }
    if (has_signature) {
      prop.has_signature = true;
      if (!r.string(&prop.signature)) {
        out.error = "malformed payload (property signature)";
        return out;
      }
    }
    out.properties.push_back(std::move(prop));
  }
  out.ok = true;
  return out;
}

} // namespace mc::server
