// SPDX-License-Identifier: GPL-3.0-or-later
#include "server/online_auth.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/log.h"
#include "net/socket_compat.h"

namespace mc::server {

// The socket layer lives under mc::net; this file only needs its transport
// vocabulary, so name those pieces rather than pulling the whole namespace in.
using net::close_socket;
using net::ensure_sockets;
using net::is_connect_in_progress;
using net::is_interrupted;
using net::kInvalidSocket;
using net::kSendFlags;
using net::last_socket_error;
using net::recv_bytes;
using net::select_nfds;
using net::send_bytes;
using net::set_nonblocking;
using net::set_receive_timeout_ms;
using net::set_send_timeout_ms;
using net::socket_len_t;
using net::socket_ssize_t;
using net::socket_startup_error;
using net::socket_t;
using net::socket_valid;

namespace {

// Small RAII helpers around OpenSSL objects.
struct PKeyDeleter {
  void operator()(EVP_PKEY* p) const {
    EVP_PKEY_free(p);
  }
};
struct CtxDeleter {
  void operator()(EVP_PKEY_CTX* p) const {
    EVP_PKEY_CTX_free(p);
  }
};
using PKeyPtr = std::unique_ptr<EVP_PKEY, PKeyDeleter>;
using PKeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, CtxDeleter>;

std::string openssl_error_string() {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
  return buf;
}

// URL-encodes a path/query segment (keeps [A-Za-z0-9-._~]).
std::string url_encode(const std::string& s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '.' || c == '_' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

struct Url {
  bool tls = false;
  std::string host;
  uint16_t port = 0;
  std::string path;
};

// Parses http(s)://host[:port]/path. Returns nullopt on garbage.
std::optional<Url> parse_url(const std::string& text) {
  Url url;
  std::string rest;
  if (text.rfind("https://", 0) == 0) {
    url.tls = true;
    url.port = 443;
    rest = text.substr(8);
  } else if (text.rfind("http://", 0) == 0) {
    url.port = 80;
    rest = text.substr(7);
  } else {
    return std::nullopt;
  }
  const size_t slash = rest.find('/');
  std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
  url.path = slash == std::string::npos ? "/" : rest.substr(slash);
  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos && hostport.find(':') == colon) {
    url.host = hostport.substr(0, colon);
    const long p = std::strtol(hostport.substr(colon + 1).c_str(), nullptr, 10);
    if (p <= 0 || p > 65535)
      return std::nullopt;
    url.port = static_cast<uint16_t>(p);
  } else {
    url.host = hostport;
  }
  if (url.host.empty())
    return std::nullopt;
  return url;
}

// How long one session-server request may take, connect included. Vanilla's
// HTTP client bounds its own request too, and the point here is the player's
// login: a session server that accepts the connection and then goes quiet must
// fail the login the same way an unreachable one does, not pin the session
// thread (and the client waiting on it) until the TCP stack gives up.
constexpr int kAuthTimeoutSeconds = 10;

// connect(2) with a deadline. The socket is switched to non-blocking for the
// connect and back afterwards, so a dropped route or a peer that never
// completes the handshake cannot hang the caller.
bool connect_with_timeout(socket_t fd, const sockaddr* addr, socket_len_t addr_len) {
  if (!set_nonblocking(fd, true))
    return false;
  bool connected = false;
  if (::connect(fd, addr, addr_len) == 0) {
    connected = true;
  } else if (is_connect_in_progress(last_socket_error())) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(fd, &writable);
    timeval tv{};
    tv.tv_sec = kAuthTimeoutSeconds;
    if (select(select_nfds(fd), nullptr, &writable, nullptr, &tv) > 0) {
      int socket_error = 0;
      socket_len_t len = sizeof(socket_error);
      connected =
          getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &len) == 0 &&
          socket_error == 0;
    }
  }
  // Back to blocking on the way out, whether or not the connect landed: the
  // caller closes the socket on failure, and the request path below expects a
  // blocking socket.
  set_nonblocking(fd, false);
  return connected;
}

// SO_RCVTIMEO / SO_SNDTIMEO, the second half of the same bound: a read or write
// that stalls gives up instead of waiting for the peer forever. The deadline in
// the request loop is what keeps a trapped OpenSSL WANT_READ from spinning.
void set_socket_timeout(socket_t fd, int seconds) {
  set_receive_timeout_ms(fd, seconds * 1000);
  set_send_timeout_ms(fd, seconds * 1000);
}

// Minimal blocking HTTP/1.1 client over a connected fd: sends `request`, reads
// the full response (Content-Length when present, else until EOF), returns
// the status code and body. `tls` wraps the fd in a TLS client connection.
struct HttpResponse {
  long status = 0;
  std::string body;
  std::string error;
};

HttpResponse http_request(const Url& url, const std::string& request) {
  HttpResponse resp;
  if (!ensure_sockets()) {
    resp.error = "cannot start the socket layer: " + socket_startup_error();
    return resp;
  }
  SSL_CTX* ssl_ctx = nullptr;
  SSL* ssl = nullptr;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(kAuthTimeoutSeconds);

  // Resolve + connect (dotted IPv4/IPv6 not assumed; use getaddrinfo).
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  const std::string port_str = std::to_string(url.port);
  if (getaddrinfo(url.host.c_str(), port_str.c_str(), &hints, &res) != 0) {
    resp.error = "resolve failed";
    return resp;
  }
  socket_t fd = kInvalidSocket;
  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (!socket_valid(fd))
      continue;
    if (connect_with_timeout(fd, ai->ai_addr, static_cast<socket_len_t>(ai->ai_addrlen)))
      break;
    close_socket(fd);
    fd = kInvalidSocket;
  }
  freeaddrinfo(res);
  if (!socket_valid(fd)) {
    resp.error = "connect failed";
    return resp;
  }
  // A session server that accepts the connection and then stops talking must
  // not pin this login thread forever: the same bound applies to the reads and
  // writes below. Vanilla's HTTP client has its own request timeout.
  set_socket_timeout(fd, kAuthTimeoutSeconds);

  auto cleanup = [&] {
    if (ssl)
      SSL_free(ssl);
    if (ssl_ctx)
      SSL_CTX_free(ssl_ctx);
    close_socket(fd);
  };

  auto read_some = [&](uint8_t* buf, size_t n) -> socket_ssize_t {
    if (ssl)
      return SSL_read(ssl, buf, static_cast<int>(n));
    return recv_bytes(fd, buf, n, 0);
  };
  auto write_all = [&](const char* data, size_t n) -> bool {
    size_t sent = 0;
    while (sent < n) {
      socket_ssize_t r;
      if (ssl) {
        r = SSL_write(ssl, data + sent, static_cast<int>(n - sent));
      } else {
        r = send_bytes(fd, data + sent, n - sent, kSendFlags);
      }
      if (r > 0) {
        sent += static_cast<size_t>(r);
        continue;
      }
      if (r < 0) {
        const int err = ssl ? SSL_get_error(ssl, static_cast<int>(r)) : last_socket_error();
        if (!ssl && is_interrupted(err))
          continue;
        // A blocking socket should not answer WANT_READ/WANT_WRITE, but the
        // socket timeout makes it possible, and a peer that is not reading is
        // exactly the case this bound exists for: retry only while there is
        // time left.
        if (ssl && (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
            std::chrono::steady_clock::now() < deadline)
          continue;
      }
      return false;
    }
    return true;
  };

  if (url.tls) {
    ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx) {
      resp.error = openssl_error_string();
      cleanup();
      return resp;
    }
    // The certificate chain and the host name are both checked, which is what
    // the vanilla server gets from the JVM's trust store. Without this the TLS
    // connection proves only that *someone* completed a handshake: an on-path
    // attacker could answer hasJoined with any name and uuid and this server
    // would hand out that identity. The default OpenSSL paths are the system
    // bundle (and, for Homebrew's OpenSSL, its own CA file).
    if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
      resp.error = "cannot load the CA store: " + openssl_error_string();
      cleanup();
      return resp;
    }
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
    ssl = SSL_new(ssl_ctx);
    // OpenSSL's public socket API takes an `int` on every platform, including
    // Windows where SOCKET is a pointer-sized handle. Handles that large are not
    // expected on a login path, but truncating one would silently wrap to a
    // different socket, so refuse rather than guess.
    if (!ssl || fd > static_cast<socket_t>(std::numeric_limits<int>::max()) ||
        SSL_set_fd(ssl, static_cast<int>(fd)) != 1 ||
        SSL_set_tlsext_host_name(ssl, url.host.c_str()) != 1) {
      resp.error = "TLS setup failed";
      cleanup();
      return resp;
    }
    X509_VERIFY_PARAM* verify = SSL_get0_param(ssl);
    X509_VERIFY_PARAM_set_hostflags(verify, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (X509_VERIFY_PARAM_set1_host(verify, url.host.c_str(), url.host.size()) != 1) {
      resp.error = "TLS host name check failed to configure";
      cleanup();
      return resp;
    }
    if (SSL_connect(ssl) != 1) {
      resp.error = "TLS handshake failed: " + openssl_error_string();
      cleanup();
      return resp;
    }
    // SSL_VERIFY_PEER already fails SSL_connect on a bad chain, but the result
    // is checked anyway so a build whose verify mode was changed elsewhere
    // cannot silently accept anybody.
    const long verify_result = SSL_get_verify_result(ssl);
    if (verify_result != X509_V_OK) {
      resp.error = std::string("TLS certificate verification failed: ") +
                   X509_verify_cert_error_string(verify_result);
      cleanup();
      return resp;
    }
  }

  if (!write_all(request.data(), request.size())) {
    resp.error = "send failed";
    cleanup();
    return resp;
  }

  // Read until EOF or Content-Length satisfied. The response is bounded: the
  // session server's answers are a few hundred bytes, and a peer that streams
  // without end (or lies about Content-Length) must not grow this process's
  // heap. The TLS peer is certificate-checked, but the bound is cheap and the
  // login path is not the place to trust a remote length.
  constexpr size_t kMaxResponseBytes = 64 * 1024;
  std::string raw;
  long content_length = -1;
  uint8_t buf[4096];
  bool headers_done = false;
  while (true) {
    socket_ssize_t r = read_some(buf, sizeof(buf));
    if (r > 0) {
      if (raw.size() + static_cast<size_t>(r) > kMaxResponseBytes) {
        resp.error = "response too large";
        cleanup();
        return resp;
      }
      raw.append(reinterpret_cast<char*>(buf), static_cast<size_t>(r));
    } else if (r == 0) {
      break; // server closed (Connection: close)
    } else {
      if (ssl) {
        const int err = SSL_get_error(ssl, static_cast<int>(r));
        const bool retryable = err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE;
        if (retryable && std::chrono::steady_clock::now() < deadline)
          continue;
        resp.error = retryable ? "request timed out" : "read failed";
        cleanup();
        return resp;
      } else if (is_interrupted(last_socket_error())) {
        continue;
      }
      resp.error = "read failed";
      cleanup();
      return resp;
    }
    if (!headers_done) {
      const size_t hd = raw.find("\r\n\r\n");
      if (hd != std::string::npos) {
        headers_done = true;
        const std::string headers = raw.substr(0, hd);
        const size_t status_line_end = headers.find("\r\n");
        const std::string status_line =
            status_line_end == std::string::npos ? headers : headers.substr(0, status_line_end);
        // "HTTP/1.1 200 OK"
        const size_t sp = status_line.find(' ');
        resp.status =
            (sp == std::string::npos) ? 0 : std::strtol(status_line.c_str() + sp + 1, nullptr, 10);
        // Content-Length (case-insensitive scan).
        std::string lower = headers;
        for (char& c : lower)
          if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        const size_t cl = lower.find("content-length:");
        if (cl != std::string::npos)
          content_length = std::strtol(lower.c_str() + cl + 15, nullptr, 10);
      }
    }
    if (headers_done && content_length >= 0) {
      const size_t body_start = raw.find("\r\n\r\n") + 4;
      const size_t have = raw.size() - body_start;
      if (have >= static_cast<size_t>(content_length)) {
        resp.body = raw.substr(body_start, static_cast<size_t>(content_length));
        cleanup();
        return resp;
      }
    }
  }
  cleanup();

  if (!headers_done) {
    resp.error = "no response headers";
    return resp;
  }
  const size_t body_start = raw.find("\r\n\r\n") + 4;
  if (content_length < 0)
    resp.body = raw.substr(body_start); // connection closed: whole body arrived
  else
    resp.body = raw.substr(body_start, static_cast<size_t>(content_length));
  return resp;
}

// Locates the value of a JSON string key at the top level of `text` beginning
// the search at `from`. Returns (value, index one past the closing quote) or
// nullopt when absent. Skips whitespace; requires "key": "value".
struct JsonString {
  std::string value;
  size_t end = 0;
};
std::optional<JsonString> json_key_string(const std::string& text, const std::string& key,
                                          size_t from = 0) {
  std::string needle = "\"" + key + "\"";
  size_t p = text.find(needle, from);
  if (p == std::string::npos)
    return std::nullopt;
  p = text.find(':', p + needle.size());
  if (p == std::string::npos)
    return std::nullopt;
  ++p;
  while (p < text.size() &&
         (text[p] == ' ' || text[p] == '\t' || text[p] == '\r' || text[p] == '\n'))
    ++p;
  if (p >= text.size() || text[p] != '"')
    return std::nullopt;
  ++p;
  std::string value;
  while (p < text.size()) {
    const char c = text[p];
    if (c == '\\') {
      if (p + 1 >= text.size())
        return std::nullopt;
      value.push_back(text[p + 1]); // \" \\ \/ \n \t ... good enough for profiles
      p += 2;
    } else if (c == '"') {
      return JsonString{value, p + 1};
    } else {
      value.push_back(c);
      ++p;
    }
  }
  return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Randomness
// ---------------------------------------------------------------------------

bool mc_random_bytes(uint8_t* out, size_t n) {
  return RAND_bytes(out, static_cast<int>(n)) == 1;
}

// ---------------------------------------------------------------------------
// ServerKeys
// ---------------------------------------------------------------------------

struct ServerKeys::Impl {
  PKeyPtr key;
  std::vector<uint8_t> pub_der;
  size_t modulus_bytes = 0;

  explicit Impl(PKeyPtr&& k) : key(std::move(k)) {
    // Serialize the public key as DER SubjectPublicKeyInfo.
    const int der_len = i2d_PUBKEY(key.get(), nullptr);
    if (der_len > 0) {
      pub_der.resize(static_cast<size_t>(der_len));
      uint8_t* cursor = pub_der.data();
      i2d_PUBKEY(key.get(), &cursor);
    }
    // Modulus size (in bytes) decides the ciphertext block size.
    if (EVP_PKEY_get_bits(key.get()) > 0)
      modulus_bytes = static_cast<size_t>((EVP_PKEY_get_bits(key.get()) + 7) / 8);
  }
};

ServerKeys::ServerKeys() {
  // Generate one 1024-bit RSA key (the size vanilla uses) with exponent 65537.
  PKeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) != 1)
    return;
  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 1024) != 1)
    return;
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &raw) != 1 || !raw) {
    if (raw)
      EVP_PKEY_free(raw);
    return;
  }
  impl_ = std::make_unique<Impl>(PKeyPtr(raw));
}

ServerKeys::~ServerKeys() = default;

const ServerKeys& ServerKeys::instance() {
  static const ServerKeys keys; // thread-safe magic static; generated once
  return keys;
}

const std::vector<uint8_t>& ServerKeys::public_key_der() const {
  static const std::vector<uint8_t> kEmpty;
  return impl_ ? impl_->pub_der : kEmpty;
}

size_t ServerKeys::key_size() const {
  return impl_ ? impl_->modulus_bytes : 0;
}

bool ServerKeys::decrypt(const uint8_t* in, size_t n, std::vector<uint8_t>* out) const {
  if (!impl_ || !impl_->key)
    return false;
  PKeyCtxPtr ctx(EVP_PKEY_CTX_new(impl_->key.get(), nullptr));
  if (!ctx || EVP_PKEY_decrypt_init(ctx.get()) != 1)
    return false;
  if (EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_PADDING) != 1)
    return false;
  size_t out_len = 0;
  if (EVP_PKEY_decrypt(ctx.get(), nullptr, &out_len, in, n) != 1)
    return false;
  out->resize(out_len);
  if (EVP_PKEY_decrypt(ctx.get(), out->data(), &out_len, in, n) != 1) {
    out->clear();
    return false;
  }
  out->resize(out_len);
  return true;
}

// ---------------------------------------------------------------------------
// Digest + session verification
// ---------------------------------------------------------------------------

std::string mc_auth_hex_digest(const std::string& server_id,
                               const std::vector<uint8_t>& shared_secret,
                               const std::vector<uint8_t>& public_key_der) {
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  if (!md)
    return {};
  bool ok = EVP_DigestInit_ex(md, EVP_sha1(), nullptr) == 1 &&
            EVP_DigestUpdate(md, server_id.data(), server_id.size()) == 1 &&
            EVP_DigestUpdate(md, shared_secret.data(), shared_secret.size()) == 1 &&
            EVP_DigestUpdate(md, public_key_der.data(), public_key_der.size()) == 1 &&
            EVP_DigestFinal_ex(md, digest, &digest_len) == 1;
  EVP_MD_CTX_free(md);
  if (!ok || digest_len != 20)
    return {};

  // Java BigInteger(byte[]).toString(16): the signed digest bytes are one big
  // two's-complement integer printed in base 16 — with '-' when negative and
  // WITHOUT leading zeros (the top nibble of the first nonzero byte is dropped
  // if zero). The digest buffer is EVP_MAX_MD_SIZE bytes but SHA-1 only wrote
  // digest_len (20) of them — never touch the rest (uninitialized stack).
  static constexpr char kHex[] = "0123456789abcdef";
  const size_t n = digest_len;
  std::string hex;
  if (digest[0] & 0x80) {
    hex.push_back('-');
    // Magnitude of a negative two's-complement number: invert all bits + 1.
    int carry = 1;
    for (size_t i = n; i-- > 0;) {
      const int v = (0xFF - digest[i]) + carry;
      digest[i] = static_cast<uint8_t>(v & 0xFF);
      carry = v >> 8;
    }
  }
  size_t first = 0;
  while (first < n && digest[first] == 0)
    ++first;
  if (first == n)
    return hex + "0";
  // Minimal hex: skip the leading zero nibble of the first nonzero byte.
  if (digest[first] >= 0x10)
    hex.push_back(kHex[digest[first] >> 4]);
  hex.push_back(kHex[digest[first] & 0xF]);
  for (size_t i = first + 1; i < n; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0xF]);
  }
  return hex;
}

// Hex-decodes the dashless UUID from hasJoined ("id") into raw 16 bytes.
bool parse_uuid_hex(const std::string& text, uint8_t out[16]) {
  std::string compact;
  compact.reserve(32);
  for (char c : text)
    if (c != '-')
      compact.push_back(c);
  if (compact.size() != 32)
    return false;
  for (size_t i = 0; i < 16; ++i) {
    auto nib = [&](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    const int hi = nib(compact[2 * i]);
    const int lo = nib(compact[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

AuthResult query_has_joined(const std::string& username, const std::string& hash) {
  AuthResult result;
  const char* env_url = std::getenv("PLUSMC_AUTH_URL");
  const std::string base = env_url ? env_url
                                   : "https://sessionserver.mojang.com/session/minecraft/"
                                     "hasJoined";
  std::optional<Url> url = parse_url(base);
  if (!url) {
    result.error = "bad PLUSMC_AUTH_URL: " + base;
    return result;
  }
  const std::string separator = url->path.find('?') == std::string::npos ? "?" : "&";
  const std::string request = "GET " + url->path + separator + "username=" + url_encode(username) +
                              "&serverId=" + url_encode(hash) +
                              " HTTP/1.1\r\n"
                              "Host: " +
                              url->host +
                              "\r\n"
                              "Connection: close\r\n"
                              "User-Agent: plusmc\r\n"
                              "\r\n";

  const HttpResponse resp = http_request(*url, request);
  if (!resp.error.empty()) {
    result.error = resp.error;
    return result;
  }
  if (resp.status == 200) {
    // {"id":"...","name":"...","properties":[{...}]}
    auto id = json_key_string(resp.body, "id");
    auto name = json_key_string(resp.body, "name");
    if (!id || !name) {
      result.error = "hasJoined 200 without id/name";
      return result;
    }
    MojangProfile& profile = result.profile;
    profile.name = name->value;
    if (!parse_uuid_hex(id->value, profile.uuid)) {
      result.error = "hasJoined returned a malformed uuid";
      return result;
    }
    // properties: iterate the array's objects.
    const size_t props_at = resp.body.find("\"properties\"");
    if (props_at != std::string::npos) {
      const size_t arr = resp.body.find('[', props_at);
      if (arr != std::string::npos) {
        size_t cur = arr + 1;
        while (cur < resp.body.size()) {
          const size_t ob = resp.body.find('{', cur);
          if (ob == std::string::npos)
            break;
          const size_t cb = resp.body.find('}', ob);
          if (cb == std::string::npos)
            break;
          const std::string obj = resp.body.substr(ob, cb - ob + 1);
          auto pn = json_key_string(obj, "name");
          auto pv = json_key_string(obj, "value");
          if (pn && pv) {
            ProfileProperty prop;
            prop.name = pn->value;
            prop.value = pv->value;
            if (auto sig = json_key_string(obj, "signature")) {
              prop.has_signature = true;
              prop.signature = sig->value;
            }
            profile.properties.push_back(std::move(prop));
          }
          cur = cb + 1;
        }
      }
    }
    result.check = AuthCheck::Ok;
    return result;
  }
  if (resp.status == 204 || resp.status == 404) {
    result.check = AuthCheck::NotJoined;
    return result;
  }
  result.error = "hasJoined status " + std::to_string(resp.status);
  return result;
}

} // namespace mc::server
