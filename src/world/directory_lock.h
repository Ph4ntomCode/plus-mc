// SPDX-License-Identifier: GPL-3.0-or-later
//
// The world's own lock (Phase 5 ops).
//
// `net.minecraft.util.DirectoryLock` (26.2) is how vanilla keeps one process to
// a level: `LevelStorageSource.LevelStorageAccess` creates it in its
// constructor, every write path calls `checkLock()` first, and `close()`
// releases it. The lock is `<level dir>/session.lock`, opened for write, filled
// with the three-byte UTF-8 snowman (`DUMMY` in the decompiled class), forced,
// and then taken with a non-blocking exclusive `FileLock`. A second process
// gets `LockException.alreadyLocked(lockPath)` — the message this port prints
// verbatim. `DirectoryLock.isLocked(dir)` is the probe: open the file for write
// (no create), `tryLock`, release if it succeeded; a missing file means
// unlocked, an access denial means locked.
//
// This port had no such file: two servers could open the same `--world` and
// write over each other, and `plusmc-restore` could only *warn* that the world
// must be closed (`docs/PHASE5.md` §5 recorded that as a limit). The same lock
// closes both: the server holds it for the process's life (across the startup
// restore drill, so nothing can slip in between), and the restore command asks
// `is_locked` before it overwrites anything.
//
// The lock is advisory, like Java's: a process that ignores the file can still
// write the world — this is a protocol between this server and this tree's
// tools, not a filesystem permission. It is also per-process on POSIX, so
// `is_locked` must never be called by the process holding the lock: closing the
// probe's descriptor would drop every POSIX record lock that process holds on
// the file. The holder only ever calls `close()`.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mc::world {

// `DirectoryLock.LOCK_FILE`.
inline constexpr const char* kWorldLockFileName = "session.lock";

// The exclusive lock a level is opened under. Move-only, released on
// destruction; a default-constructed value holds nothing.
class DirectoryLock {
public:
  // `DirectoryLock.create`. Creates `dir` when missing (vanilla's
  // `FileUtil.createDirectoriesSafe`), writes the dummy contents, and takes the
  // lock. On failure returns nullopt and, when `why` is not null, the refusal
  // in vanilla's own words (`<path>: already locked (possibly by other
  // Minecraft instance?)` for the contended case).
  static std::optional<DirectoryLock> acquire(const std::filesystem::path& dir, std::string* why);

  // `DirectoryLock.isLocked`: whether another process holds the world's lock.
  // Never call this from the process holding it (see the note above).
  static bool is_locked(const std::filesystem::path& dir);

  DirectoryLock() = default;
  ~DirectoryLock();
  DirectoryLock(DirectoryLock&& other) noexcept;
  DirectoryLock& operator=(DirectoryLock&& other) noexcept;
  DirectoryLock(const DirectoryLock&) = delete;
  DirectoryLock& operator=(const DirectoryLock&) = delete;

  // `FileLock.isValid()`: true until `close()` (or the process ends).
  bool is_valid() const {
    return valid_;
  }
  void close();

private:
  explicit DirectoryLock(std::intptr_t handle) : handle_(handle), valid_(true) {}

  // A POSIX file descriptor or a Win32 HANDLE, held for the lock's lifetime.
  std::intptr_t handle_ = -1;
  bool valid_ = false;
};

} // namespace mc::world
