// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte-transport abstraction. Protocol framing (src/protocol/framing.h)
// reads/writes frames through a Stream, so a session can switch transports
// mid-flight: online-mode logins start over a raw Connection and, once the
// AES/CFB8 shared secret is agreed, everything after the Encryption Response
// flows through a CipherStream wrapping the same Connection. Keeping the
// cipher state inside a Stream object (instead of stateless per-call
// transforms) is required: CFB8 feedback state persists across calls.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mc::net {

class Stream {
public:
  virtual ~Stream() = default;

  // Reads exactly `n` bytes. Returns false on EOF, timeout, or error.
  virtual bool read_exact(uint8_t* out, size_t n) = 0;
  // Writes all `n` bytes. Returns false on error / closed peer.
  virtual bool write_all(const uint8_t* data, size_t n) = 0;
};

} // namespace mc::net
