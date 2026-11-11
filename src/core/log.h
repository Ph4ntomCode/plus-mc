// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal thread-safe logger for the +MC engine. Messages go to stderr so
// stdout stays free for machine-readable output (e.g. "listening on ...").
#pragma once

#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

namespace mc::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error };

inline Level& current_level() {
  static Level level = Level::Info;
  return level;
}

inline void set_level(Level level) {
  current_level() = level;
}

inline bool enabled(Level level) {
  return level >= current_level();
}

inline void emit(Level level, std::string message) {
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);
  const char* tag = "debug";
  switch (level) {
  case Level::Trace:
    tag = "trace";
    break;
  case Level::Debug:
    tag = "debug";
    break;
  case Level::Info:
    tag = "info ";
    break;
  case Level::Warn:
    tag = "warn ";
    break;
  case Level::Error:
    tag = "error";
    break;
  }
  std::fprintf(stderr, "[%s] %s\n", tag, message.c_str());
}

namespace detail {
template <typename... Args> inline void compose(Level level, Args&&... args) {
  if (!enabled(level))
    return;
  std::ostringstream os;
  (os << ... << args);
  emit(level, os.str());
}
} // namespace detail

template <typename... Args> void trace(Args&&... args) {
  detail::compose(Level::Trace, std::forward<Args>(args)...);
}
template <typename... Args> void debug(Args&&... args) {
  detail::compose(Level::Debug, std::forward<Args>(args)...);
}
template <typename... Args> void info(Args&&... args) {
  detail::compose(Level::Info, std::forward<Args>(args)...);
}
template <typename... Args> void warn(Args&&... args) {
  detail::compose(Level::Warn, std::forward<Args>(args)...);
}
template <typename... Args> void error(Args&&... args) {
  detail::compose(Level::Error, std::forward<Args>(args)...);
}

} // namespace mc::log
