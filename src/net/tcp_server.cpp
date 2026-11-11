// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/tcp_server.h"

#include <chrono>
#include <cstring>

#include <algorithm>
#include <system_error>
#include <thread>
#include <utility>

#include "core/log.h"

namespace mc::net {

namespace {

// `Connection.configureSerialization`: vanilla puts a netty `ReadTimeoutHandler`
// of 30 seconds on every channel, so half a minute of silence is what ends a
// session there. This server's own keep-alive is the *other* half of that
// arrangement (15 s apart in vanilla, and a session whose keep-alive goes
// unanswered is dropped when the next one is due) — the read timeout only has to
// be longer than the client's chance to answer one. That is why the floor lives
// on Connection rather than being applied here: the accept loop uses
// `Options::receive_timeout_ms`, which main() derives from the configured
// keep-alive, so a tuned cadence moves this backstop with it instead of reaping
// healthy-but-silent sessions first.
// The write bound has no vanilla counterpart and is not a timeout in the same
// sense: nothing legitimate holds a send for 15 seconds, and a peer that stops
// reading must not be able to stall a write (and, with it, the shared broadcast
// machinery).
constexpr int kSendTimeoutSeconds = 15;
constexpr int kListenBacklog = 128;
// How long shutdown() waits for in-flight session threads after force-closing
// their sockets (see tcp_server.h). A session handles the resulting EOF in
// milliseconds, so this bounds a wedged handler rather than describing a normal
// wait; a thread still running at the bound is logged and left to the process.
constexpr int kConnectionDrainSeconds = 10;

} // namespace

Connection::Connection(socket_t fd, int receive_timeout_ms) : fd_(fd) {
  // A zero/negative SO_RCVTIMEO means "wait forever" to the kernel, which would
  // turn the wrong option into a session that can never be reaped.
  if (receive_timeout_ms <= 0)
    receive_timeout_ms = kDefaultReceiveTimeoutMs;
  set_receive_timeout_ms(fd_, receive_timeout_ms);
  set_send_timeout_ms(fd_, kSendTimeoutSeconds * 1000);
}

Connection::~Connection() {
  close();
}

// One `recv` into the read-ahead buffer. Returns false on EOF, timeout or
// error, which is the same contract `read_exact` had before the buffer: the
// caller cannot tell whether the bytes came from the kernel in this call or an
// earlier one, which is exactly what makes the change invisible to every layer
// above (framing, the cipher, the login handshake).
bool Connection::fill_read_buffer() {
  read_pos_ = 0;
  read_len_ = 0;
  for (;;) {
    const socket_ssize_t r = recv_bytes(fd_, read_buffer_.data(), read_buffer_.size(), 0);
    if (r > 0) {
      read_len_ = static_cast<size_t>(r);
      return true;
    }
    if (r == 0)
      return false; // clean EOF
    if (is_interrupted(last_socket_error()))
      continue;
    // EAGAIN/EWOULDBLOCK (timeout) and other errors all end the session.
    return false;
  }
}

bool Connection::read_exact(uint8_t* out, size_t n) {
  size_t got = 0;
  // Whatever the last fill already read comes first.
  if (read_pos_ < read_len_) {
    const size_t take = std::min(n, read_len_ - read_pos_);
    std::memcpy(out, read_buffer_.data() + read_pos_, take);
    read_pos_ += take;
    got = take;
    if (read_pos_ == read_len_)
      read_pos_ = read_len_ = 0;
  }
  while (got < n) {
    const size_t need = n - got;
    // A large request (a frame body's 16 KiB slice) is read straight into the
    // caller's buffer: going through the read-ahead buffer would copy every
    // body byte twice for no syscall saved.
    if (need >= kReadBufferBytes) {
      const socket_ssize_t r = recv_bytes(fd_, out + got, need, 0);
      if (r > 0) {
        got += static_cast<size_t>(r);
        continue;
      }
      if (r == 0)
        return false; // clean EOF
      if (is_interrupted(last_socket_error()))
        continue;
      return false;
    }
    if (!fill_read_buffer())
      return false;
    const size_t take = std::min(need, read_len_);
    std::memcpy(out + got, read_buffer_.data(), take);
    read_pos_ = take;
    got += take;
    if (read_pos_ == read_len_)
      read_pos_ = read_len_ = 0;
  }
  return true;
}

bool Connection::write_all(const uint8_t* data, size_t n) {
  // Per-connection, not global: frame ordering is a property of one socket.
  // Held across the whole frame so two writers cannot interleave bytes, and
  // released only when the frame is on its way (see tcp_server.h).
  std::lock_guard<std::mutex> lock(write_mu_);
  size_t sent = 0;
  while (sent < n) {
    const socket_ssize_t r = send_bytes(fd_, data + sent, n - sent, kSendFlags);
    if (r > 0) {
      sent += static_cast<size_t>(r);
      continue;
    }
    if (r < 0 && is_interrupted(last_socket_error()))
      continue;
    return false;
  }
  return true;
}

void Connection::close() {
  if (socket_valid(fd_)) {
    close_socket(fd_);
    fd_ = kInvalidSocket;
  }
}

TcpServer::~TcpServer() {
  shutdown();
}

bool TcpServer::start(Options options) {
  options_ = std::move(options);

  if (!ensure_sockets()) {
    log::error("cannot start the socket layer: ", socket_startup_error());
    return false;
  }

  socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!socket_valid(fd)) {
    log::error("socket(): ", socket_error_text(last_socket_error()));
    return false;
  }
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             static_cast<socket_len_t>(sizeof(reuse)));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    if (options_.host == "localhost") {
      inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    } else {
      log::error("cannot parse host '", options_.host, "' (dotted IPv4 only for now)");
      close_socket(fd);
      return false;
    }
  }

  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socket_len_t>(sizeof(addr))) != 0) {
    log::error("bind(", options_.host, ":", options_.port,
               "): ", socket_error_text(last_socket_error()));
    close_socket(fd);
    return false;
  }
  if (listen(fd, kListenBacklog) != 0) {
    log::error("listen(): ", socket_error_text(last_socket_error()));
    close_socket(fd);
    return false;
  }
  // Non-blocking so the accept thread can drain the queue and re-select; it
  // never blocks indefinitely inside accept().
  if (!set_nonblocking(fd, true)) {
    log::error("cannot make the listener non-blocking: ", socket_error_text(last_socket_error()));
    close_socket(fd);
    return false;
  }

  socket_len_t len = sizeof(addr);
  getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);

  if (!make_wake_pair(&wake_read_, &wake_write_)) {
    log::error("cannot create the shutdown wake pair: ", socket_error_text(last_socket_error()));
    close_socket(fd);
    return false;
  }

  running_ = true;
  listen_fd_.store(fd);
  try {
    accept_thread_ = std::thread([this] { accept_loop(); });
  } catch (const std::system_error& e) {
    log::error("cannot start accept thread: ", e.what());
    running_ = false;
    listen_fd_.store(kInvalidSocket);
    close_socket(fd);
    close_socket(wake_read_);
    close_socket(wake_write_);
    wake_read_ = kInvalidSocket;
    wake_write_ = kInvalidSocket;
    return false;
  }
  return true;
}

void TcpServer::accept_loop() {
  auto handler = options_.handler;
  const int receive_timeout_ms = options_.receive_timeout_ms;
  socket_t listen_fd = listen_fd_.load();

  while (running_) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd, &rfds);
    FD_SET(wake_read_, &rfds);
    const int max_fd = select_max_nfds(listen_fd, wake_read_);

    int r = select(max_fd, &rfds, nullptr, nullptr, nullptr);
    if (r < 0) {
      const int error = last_socket_error();
      if (is_interrupted(error))
        continue;
      log::error("select(): ", socket_error_text(error), " — stopping accept loop");
      break;
    }
    if (FD_ISSET(wake_read_, &rfds))
      break; // shutdown() asked us to stop

    if (FD_ISSET(listen_fd, &rfds)) {
      // Drain the ready queue; accept() is non-blocking so it returns EAGAIN
      // when drained and we loop back to select().
      for (;;) {
        sockaddr_in peer{};
        socket_len_t peer_len = sizeof(peer);
        socket_t client = accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (!socket_valid(client)) {
          const int error = last_socket_error();
          if (is_would_block(error))
            break;
          if (is_interrupted(error) || is_connection_aborted(error))
            continue;
          // Persistent failures (e.g. EMFILE under fd exhaustion) must not
          // turn into a hot spin.
          log::error("accept(): ", socket_error_text(error), " — backing off");
          std::this_thread::sleep_for(std::chrono::milliseconds(25));
          break;
        }
        // BSD/macOS accept() inherits O_NONBLOCK from the listening socket
        // (Linux does not); Connection is a blocking API, so clear it or an
        // idle recv() returns EAGAIN instantly and drops the session. Windows
        // accepted sockets are blocking already, and doing this uniformly keeps
        // one code path instead of a platform branch.
        set_nonblocking(client, false);
        // Register the connection *before* its thread exists, and erase it and
        // close the socket together under `conns_mu_`. That pairing is what makes
        // the registry trustworthy: while a socket is in `live_conns_` it is open
        // and owned by a running handler thread, so shutdown() may force it
        // closed and then wait for the set to empty (see shutdown()).
        {
          std::lock_guard<std::mutex> lock(conns_mu_);
          live_conns_.insert(client);
        }
        try {
          std::thread([client, handler, this, receive_timeout_ms] {
            Connection conn(client, receive_timeout_ms);
            handler(conn);
            {
              std::lock_guard<std::mutex> lock(conns_mu_);
              live_conns_.erase(client);
              conn.close();
            }
            conns_cv_.notify_all();
          }).detach();
        } catch (const std::system_error& e) {
          // Keep the registry honest if the thread could not start: a socket
          // left in it would make shutdown() wait out its bound for a thread
          // that never ran.
          log::error("cannot start session thread: ", e.what());
          {
            std::lock_guard<std::mutex> lock(conns_mu_);
            live_conns_.erase(client);
          }
          conns_cv_.notify_all();
          close_socket(client);
        }
      }
    }
  }

  // Ownership split (avoids a data race on the wake handles): the accept thread
  // closes only wake_read_ (its own select handle). shutdown() writes to and
  // then closes wake_write_ after joining, so no two threads ever touch the
  // same handle or the same plain-handle member concurrently.
  close_socket(listen_fd);
  listen_fd_.store(kInvalidSocket);
  if (socket_valid(wake_read_)) {
    close_socket(wake_read_);
    wake_read_ = kInvalidSocket;
  }
}

void TcpServer::shutdown() {
  if (!running_.exchange(false))
    return;
  // Wake the accept thread out of select(). If it already exited (e.g. on a
  // select() error) the write lands harmlessly in the wake pair and join()
  // returns immediately; wake_write_ is closed only here, after the thread is
  // done.
  signal_wake_pair(wake_write_);
  if (accept_thread_.joinable())
    accept_thread_.join();
  if (socket_valid(wake_write_)) {
    close_socket(wake_write_);
    wake_write_ = kInvalidSocket;
  }

  // Drain the session threads. main() returns as soon as this function does,
  // and the handler threads use the engine's namespace-scope mutexes and the
  // world; letting them outlive main() means static destruction destroys those
  // under them (SIGABRT, "mutex lock failed: Invalid argument" — the 200-bot
  // mover soak hit it, and a host's Ctrl-C reaches it with far fewer players).
  // The accept thread is already joined, so nothing new can register.
  //
  // Two passes, because the two ways a session parks are woken differently:
  //  - `ShutdownHow::Read` first: a session parked in recv() sees the ordinary
  //    EOF and runs the teardown it runs for a disconnecting client. That is
  //    the clean path — it is what leaves the session's own `disconnected` line
  //    in the log, so a run can be read back afterwards.
  //  - `ShutdownHow::Both` for whatever is left: a session parked in a write (a
  //    client that stopped reading, a broadcast backlog) is not woken by the
  //    read half, and this is the part that actually guarantees no session
  //    outlives main().
  //
  // `shutdown_socket` and not `close`: the socket is the handler thread's, and
  // the thread closes it under `conns_mu_` as it unregisters, so an entry in
  // the set is always open and its handle cannot have been reused.
  {
    std::lock_guard<std::mutex> lock(conns_mu_);
    for (socket_t fd : live_conns_)
      shutdown_socket(fd, ShutdownHow::Read);
  }
  {
    std::unique_lock<std::mutex> lock(conns_mu_);
    conns_cv_.wait_for(lock, std::chrono::seconds(kConnectionDrainSeconds),
                       [this] { return live_conns_.empty(); });
  }
  {
    std::lock_guard<std::mutex> lock(conns_mu_);
    for (socket_t fd : live_conns_)
      shutdown_socket(fd, ShutdownHow::Both);
  }
  std::unique_lock<std::mutex> lock(conns_mu_);
  if (!conns_cv_.wait_for(lock, std::chrono::seconds(kConnectionDrainSeconds),
                          [this] { return live_conns_.empty(); })) {
    log::warn("shutdown: ", live_conns_.size(),
              " session thread(s) still running; they end with the process");
  }
}

} // namespace mc::net
