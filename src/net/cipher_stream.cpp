// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/cipher_stream.h"

#include <openssl/evp.h>

#include <cstdint>
#include <mutex>
#include <vector>

namespace mc::net {

struct CipherStream::Impl {
  Stream& raw;
  EVP_CIPHER_CTX* enc = nullptr;
  EVP_CIPHER_CTX* dec = nullptr;
  // Encrypt-then-write is one critical section: the CFB8 state is shared by
  // every writer of this connection (the session thread, the tick's relay
  // paths, the keep-alive thread), and a race here does not merely interleave
  // bytes — it corrupts the keystream for every later frame. The read side has
  // its own context and its own single reader, so it needs no lock.
  std::mutex write_mu;
  // The encryption scratch, reused across writes. `EVP_EncryptUpdate` for
  // CFB8 produces exactly as many bytes as it consumes, and the bytes are
  // handed straight to the transport, so a fresh `std::vector` per frame was
  // one allocation per write for a buffer whose size never changes. Guarded by
  // `write_mu` like the cipher context it feeds.
  std::vector<uint8_t> scratch;

  Impl(Stream& raw_, const uint8_t secret[16]) : raw(raw_) {
    enc = EVP_CIPHER_CTX_new();
    dec = EVP_CIPHER_CTX_new();
    if (!enc || !dec)
      return; // contexts stay null; every call then fails closed
    const EVP_CIPHER* cipher = EVP_aes_128_cfb8();
    // Key and IV are both the shared secret (vanilla behaviour).
    EVP_EncryptInit_ex(enc, cipher, nullptr, secret, secret);
    EVP_DecryptInit_ex(dec, cipher, nullptr, secret, secret);
  }
  ~Impl() {
    if (enc)
      EVP_CIPHER_CTX_free(enc);
    if (dec)
      EVP_CIPHER_CTX_free(dec);
  }
};

CipherStream::CipherStream(Stream& raw, const uint8_t secret[16])
    : impl_(std::make_unique<Impl>(raw, secret)) {}

CipherStream::~CipherStream() = default;

CipherStream::CipherStream(CipherStream&&) noexcept = default;
CipherStream& CipherStream::operator=(CipherStream&&) noexcept = default;

bool CipherStream::read_exact(uint8_t* out, size_t n) {
  if (!impl_ || !impl_->dec)
    return false;
  if (!impl_->raw.read_exact(out, n))
    return false;
  int produced = 0;
  if (EVP_DecryptUpdate(impl_->dec, out, &produced, out, static_cast<int>(n)) != 1)
    return false;
  return static_cast<size_t>(produced) == n;
}

bool CipherStream::write_all(const uint8_t* data, size_t n) {
  if (!impl_ || !impl_->enc)
    return false;
  std::lock_guard<std::mutex> lock(impl_->write_mu);
  const size_t capacity = n + EVP_MAX_BLOCK_LENGTH;
  if (impl_->scratch.size() < capacity)
    impl_->scratch.resize(capacity);
  int produced = 0;
  if (EVP_EncryptUpdate(impl_->enc, impl_->scratch.data(), &produced, data, static_cast<int>(n)) !=
      1)
    return false;
  return impl_->raw.write_all(impl_->scratch.data(), static_cast<size_t>(produced));
}

} // namespace mc::net
