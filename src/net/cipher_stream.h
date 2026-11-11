// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minecraft protocol encryption transport (online mode). Once the client and
// server share a 16-byte secret, every byte in both directions — frame length,
// packet id, data — is AES-128-CFB8 encrypted, with the secret used as both
// key and initial vector. CFB8 feedback state must persist across packets, so
// the cipher lives inside this Stream and is never reset per call.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "net/stream.h"

namespace mc::net {

class CipherStream : public Stream {
public:
  // Wraps `raw` and enables encryption for every subsequent read/write. The
  // secret is 16 bytes and is also the CFB8 initial vector (as vanilla does).
  CipherStream(Stream& raw, const uint8_t secret[16]);
  ~CipherStream() override;
  CipherStream(const CipherStream&) = delete;
  CipherStream& operator=(const CipherStream&) = delete;
  CipherStream(CipherStream&&) noexcept;
  CipherStream& operator=(CipherStream&&) noexcept;

  bool read_exact(uint8_t* out, size_t n) override;
  bool write_all(const uint8_t* data, size_t n) override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mc::net
