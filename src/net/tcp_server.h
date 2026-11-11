// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal TCP plumbing for Phase 1 (macOS, Linux, and Windows). Interim
// thread-per-connection design; the plan targets an async reactor (io_uring /
// kqueue / IOCP) before scale-out (docs/PLAN.md §6.2). The Connection API is
// deliberately narrow so the reactor swap does not touch protocol code, and
// net/socket_compat.h is the only place that knows a socket is not an `int`
// everywhere.
//
// Shutdown uses a wake pair: shutdown() writes a byte to wake the accept thread
// out of select(), which is the portable way to interrupt a blocked accept
// (calling shutdown()/close() on a listening socket from another thread does
// NOT reliably wake accept()). On POSIX that pair is a pipe; on Windows it is a
// connected loopback TCP pair (see socket_compat.h). Ownership is split so no
// two threads race on the same handle: the accept thread closes only
// wake_read_, and shutdown() writes to then closes wake_write_ after joining.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#include "net/socket_compat.h"
#include "net/stream.h"

namespace mc::net {

// A single accepted socket. Blocking reads with a receive timeout.
class Connection : public Stream {
public:
  // Vanilla's netty ReadTimeoutHandler bound (`Connection.configureSerialization`).
  // It is a *floor*, not a fixed rule: main() raises it for a server whose own
  // keep-alive cadence is longer, because a socket that times out before the
  // keep-alive does drops clients the keep-alive still considers alive.
  static constexpr int kDefaultReceiveTimeoutMs = 30000;
  explicit Connection(socket_t fd, int receive_timeout_ms = kDefaultReceiveTimeoutMs);
  ~Connection();
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Reads exactly `n` bytes. Returns false on EOF, timeout, or error.
  bool read_exact(uint8_t* out, size_t n) override;
  // Writes all `n` bytes. Returns false on error / closed peer.
  //
  // Serialized per connection: two threads writing this socket (a session
  // thread answering and a tick thread relaying) must not interleave frame
  // bytes, and there are hundreds of sockets — one mutex for all of them (the
  // server used to hold a single global write lock in session.cpp) made every
  // connection queue behind every other. The lock lives here, so every writer
  // is covered whether it goes through the engine's write() helper or calls
  // the framing layer directly. `read_exact` does not take it: only one thread
  // reads a connection, and the send side never blocks a receive.
  bool write_all(const uint8_t* data, size_t n) override;

  void close();
  socket_t fd() const {
    return fd_;
  }

private:
  // Read-ahead buffer. Framing asks for a handful of bytes at a time — the
  // frame length VarInt, the compressed `dataLength` VarInt, then the body in
  // 16 KiB slices — so a raw `recv` per request cost up to five syscalls before
  // the first body byte, per packet, and the same again inside the cipher (its
  // CFB8 loop reads one byte per call). One `recv` of this size serves every
  // request until the buffer drains, so a packet costs one syscall for its
  // framing and one per body slice instead of ~7. Body reads of a buffer's
  // size or larger still go straight to the caller's pointer: the copy is only
  // paid where it buys something.
  //
  // One thread reads a connection (the session's), so this needs no lock; it
  // sits beside `write_mu_` rather than in `Stream` because only a transport
  // that owns a socket can refill it.
  static constexpr size_t kReadBufferBytes = 16 * 1024;
  bool fill_read_buffer();

  socket_t fd_ = kInvalidSocket;
  std::mutex write_mu_;
  std::array<uint8_t, kReadBufferBytes> read_buffer_{};
  size_t read_pos_ = 0;
  size_t read_len_ = 0;
};

// Accept loop. One handler call per accepted connection; each handler runs on
// its own thread (interim design, see header comment).
class TcpServer {
public:
  struct Options {
    std::string host = "0.0.0.0"; // dotted IPv4 only for now
    uint16_t port = 25565;        // 0 => kernel-assigned port
    // Silence that ends a connection (see Connection). main() sets it from the
    // keep-alive settings so the socket backstop never preempts them.
    int receive_timeout_ms = Connection::kDefaultReceiveTimeoutMs;
    std::function<void(Connection&)> handler;
  };

  TcpServer() = default;
  ~TcpServer();
  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  // Binds and starts the accept loop. Returns false on bind/listen failure.
  bool start(Options options);
  // Actual bound port (useful when Options::port == 0).
  uint16_t port() const {
    return port_;
  }
  // Stops the accept loop, then drains the in-flight handler threads before
  // returning: it force-closes every still-registered connection (which is the
  // event each session's read loop already handles) and waits for the handler
  // threads to leave the registry.
  //
  // The wait is not a formality. The handler threads are detached and main()
  // returns straight after this call, so with sessions still running static
  // destruction would tear down the mutexes (and the world) they are still
  // using — an abort, not a clean exit. A 200-bot mover soak reproduced exactly
  // that ('mutex lock failed: Invalid argument', SIGABRT, main already back in
  // its caller); the drain is what makes the shutdown graceful at scale. If a
  // thread does not drain within the bound the old ordering is preserved and a
  // warning is logged rather than hanging a host.
  void shutdown();

private:
  void accept_loop();

  Options options_;
  uint16_t port_ = 0;
  // Atomic: written by the accept thread (closed => kInvalidSocket) and read by
  // shutdown() from another thread; a plain handle here would be a data race.
  std::atomic<socket_t> listen_fd_{kInvalidSocket};
  // Wake pair (see header comment). Owned by the accept thread.
  socket_t wake_read_ = kInvalidSocket;
  socket_t wake_write_ = kInvalidSocket;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  // The connections whose handler threads are still running, and the sockets
  // they own. Registration happens in the accept loop *before* the handler thread is
  // spawned and the erase happens under the same mutex *as* the close, so a
  // socket in this set is always open and owned by a live thread — shutdown()
  // can safely force it closed and then wait for the set to empty. Guarded by
  // conns_mu_ (which shutdown() also waits on, via conns_cv_).
  std::mutex conns_mu_;
  std::condition_variable conns_cv_;
  std::unordered_set<socket_t> live_conns_;
};

} // namespace mc::net
