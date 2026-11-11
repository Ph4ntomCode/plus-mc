// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one place that knows a socket is not an `int` on every platform.
//
// The net layer was written against POSIX: `int` fds, `fcntl(O_NONBLOCK)`,
// `errno`, `MSG_NOSIGNAL`, and a self-pipe to wake `select()` on shutdown.
// Windows has the same *operations* under different types and constants —
// `SOCKET` is a pointer-sized handle, `ioctlsocket` replaces `fcntl`,
// `WSAGetLastError` replaces `errno`, and there is no pipe that `select()`
// accepts. This header is the narrow seam between those two worlds, so
// `tcp_server.cpp` and `online_auth.cpp` stay one implementation rather than
// growing an `#ifdef` every few lines.
//
// The wake pair is the POSIX pipe on macOS/Linux and a connected loopback TCP
// pair on Windows: both ends are then sockets, `select()` works unchanged, and
// writes to a socket are the same operation shutdown() already needs. PLAN §6.2
// wants this abstraction for the future IOCP reactor too; the ownership rules
// (the accept thread owns the read end, shutdown() owns the write end) are the
// same for both transports.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
// windows.h defines min/max as macros unless NOMINMAX was in effect before it
// was first included. The engine uses std::min/std::max in these very files.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mc::net {

#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
using socket_len_t = int;
// Winsock's send/recv take an `int` length; POSIX takes size_t. This is the
// return type both can share (`ssize_t` does not exist under MSVC).
using socket_ssize_t = std::ptrdiff_t;
// Windows has no SIGPIPE, so there is no MSG_NOSIGNAL to pass. A reset peer is
// reported as a send error instead.
inline constexpr int kSendFlags = 0;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
using socket_len_t = socklen_t;
using socket_ssize_t = std::ptrdiff_t;
inline constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

inline bool socket_valid(socket_t socket) {
  return socket != kInvalidSocket;
}

// Winsock has to be started once per process before any socket call (including
// getaddrinfo). The function-local static makes that happen once, thread-safely;
// it is deliberately never paired with WSACleanup, because process exit releases
// the library and a cleanup during static destruction could race a closing
// socket. POSIX has nothing to start.
#if defined(_WIN32)
inline int winsock_startup_result() {
  static const int startup = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data);
  }();
  return startup;
}
#endif

inline bool ensure_sockets() {
#if defined(_WIN32)
  return winsock_startup_result() == 0;
#else
  return true;
#endif
}

inline int last_socket_error() {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

inline std::string socket_error_text(int error) {
#if defined(_WIN32)
  char* raw = nullptr;
  const DWORD length = ::FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&raw), 0, nullptr);
  std::string text = (length != 0 && raw != nullptr)
                         ? std::string(raw, length)
                         : std::string("winsock error ") + std::to_string(error);
  if (raw != nullptr)
    ::LocalFree(raw);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
    text.pop_back();
  return text;
#else
  return std::strerror(error);
#endif
}

inline std::string socket_startup_error() {
#if defined(_WIN32)
  const int result = winsock_startup_result();
  return result == 0 ? std::string() : socket_error_text(result);
#else
  return {};
#endif
}

inline bool is_would_block(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

inline bool is_connect_in_progress(int error) {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
#else
  return error == EINPROGRESS || error == EALREADY || error == EAGAIN || error == EWOULDBLOCK;
#endif
}

inline bool is_interrupted(int error) {
#if defined(_WIN32)
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

inline bool is_connection_aborted(int error) {
#if defined(_WIN32)
  return error == WSAECONNABORTED;
#else
  return error == ECONNABORTED;
#endif
}

// `fcntl(O_NONBLOCK)` on POSIX, `ioctlsocket(FIONBIO)` on Windows. Note that
// BSD/macOS inherits O_NONBLOCK through accept() while Windows does not; the
// accept loop clears it on the accepted socket on both, because the call is
// cheap and one unconditional path is easier to keep correct than a branch.
inline bool set_nonblocking(socket_t socket, bool enabled) {
#if defined(_WIN32)
  u_long mode = enabled ? 1ul : 0ul;
  return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
  const int flags = ::fcntl(socket, F_GETFL, 0);
  if (flags < 0)
    return false;
  return ::fcntl(socket, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

// SO_RCVTIMEO takes a timeval on POSIX and a DWORD of milliseconds on Windows;
// SO_SNDTIMEO is the same split. A zero timeout means "forever" to both
// kernels, so callers keep the value positive.
inline bool set_receive_timeout_ms(socket_t socket, int milliseconds) {
#if defined(_WIN32)
  const DWORD value = static_cast<DWORD>(milliseconds);
  return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0;
#else
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  return ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
#endif
}

inline bool set_send_timeout_ms(socket_t socket, int milliseconds) {
#if defined(_WIN32)
  const DWORD value = static_cast<DWORD>(milliseconds);
  return ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0;
#else
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  return ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0;
#endif
}

inline socket_ssize_t recv_bytes(socket_t socket, void* buffer, size_t length, int flags) {
#if defined(_WIN32)
  const int chunk = length > static_cast<size_t>(std::numeric_limits<int>::max())
                        ? std::numeric_limits<int>::max()
                        : static_cast<int>(length);
  return static_cast<socket_ssize_t>(::recv(socket, static_cast<char*>(buffer), chunk, flags));
#else
  return static_cast<socket_ssize_t>(::recv(socket, buffer, length, flags));
#endif
}

inline socket_ssize_t send_bytes(socket_t socket, const void* data, size_t length, int flags) {
#if defined(_WIN32)
  const int chunk = length > static_cast<size_t>(std::numeric_limits<int>::max())
                        ? std::numeric_limits<int>::max()
                        : static_cast<int>(length);
  return static_cast<socket_ssize_t>(::send(socket, static_cast<const char*>(data), chunk, flags));
#else
  return static_cast<socket_ssize_t>(::send(socket, data, length, flags));
#endif
}

inline void close_socket(socket_t socket) {
  if (!socket_valid(socket))
    return;
#if defined(_WIN32)
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

enum class ShutdownHow { Read, Write, Both };

inline bool shutdown_socket(socket_t socket, ShutdownHow how) {
#if defined(_WIN32)
  const int native = how == ShutdownHow::Read    ? SD_RECEIVE
                     : how == ShutdownHow::Write ? SD_SEND
                                                 : SD_BOTH;
#else
  const int native = how == ShutdownHow::Read    ? SHUT_RD
                     : how == ShutdownHow::Write ? SHUT_WR
                                                 : SHUT_RDWR;
#endif
  return ::shutdown(socket, native) == 0;
}

// select()'s first argument is the highest fd plus one on POSIX and ignored on
// Windows (where the fd_set carries the socket handles). These keep the call
// sites from spelling either rule.
inline int select_nfds(socket_t socket) {
#if defined(_WIN32)
  (void)socket;
  return 0;
#else
  return socket < 0 ? 0 : static_cast<int>(socket) + 1;
#endif
}

inline int select_max_nfds(socket_t a, socket_t b) {
#if defined(_WIN32)
  (void)a;
  (void)b;
  return 0;
#else
  const socket_t high = a > b ? a : b;
  return select_nfds(high);
#endif
}

// The shutdown wake channel. POSIX keeps the existing self-pipe; Windows uses a
// connected loopback TCP pair, because there is no pipe that `select()` accepts
// and both ends of a socket pair are ordinary sockets. `read_end` is the accept
// thread's, `write_end` is shutdown()'s (see tcp_server.h).
inline bool make_wake_pair(socket_t* read_end, socket_t* write_end) {
  *read_end = kInvalidSocket;
  *write_end = kInvalidSocket;
#if defined(_WIN32)
  socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!socket_valid(listener))
    return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  socket_len_t addr_len = sizeof(addr);
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0 ||
      ::listen(listener, 1) != 0 ||
      ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
    close_socket(listener);
    return false;
  }
  socket_t writer = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (!socket_valid(writer)) {
    close_socket(listener);
    return false;
  }
  if (::connect(writer, reinterpret_cast<sockaddr*>(&addr), addr_len) != 0) {
    close_socket(writer);
    close_socket(listener);
    return false;
  }
  socket_t reader = ::accept(listener, nullptr, nullptr);
  close_socket(listener);
  if (!socket_valid(reader)) {
    close_socket(writer);
    return false;
  }
  *read_end = reader;
  *write_end = writer;
  return true;
#else
  int fds[2];
  if (::pipe(fds) != 0)
    return false;
  *read_end = fds[0];
  *write_end = fds[1];
  return true;
#endif
}

inline void signal_wake_pair(socket_t write_end) {
  if (!socket_valid(write_end))
    return;
  const uint8_t byte = 1;
#if defined(_WIN32)
  (void)send_bytes(write_end, &byte, 1, 0);
#else
  const ssize_t ignored = ::write(write_end, &byte, 1);
  (void)ignored;
#endif
}

} // namespace mc::net
