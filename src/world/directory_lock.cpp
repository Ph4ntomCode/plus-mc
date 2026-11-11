// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/directory_lock.h"

#include <cerrno>
#include <cstring>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mc::world {
namespace {

// `DirectoryLock.DUMMY`: the snowman vanilla writes into a level's lock file.
constexpr char kDummy[] = "\xE2\x98\x83";
constexpr std::size_t kDummyBytes = 3;

std::string already_locked(const std::filesystem::path& lock_path) {
  return lock_path.string() + ": already locked (possibly by other Minecraft instance?)";
}

} // namespace

std::optional<DirectoryLock> DirectoryLock::acquire(const std::filesystem::path& dir,
                                                    std::string* why) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::is_directory(dir, ec)) {
    if (why != nullptr)
      *why = dir.string() + ": " + ec.message();
    return std::nullopt;
  }
  const std::filesystem::path lock_path = dir / kWorldLockFileName;
#if defined(_WIN32)
  HANDLE handle = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (why != nullptr)
      *why =
          lock_path.string() + ": cannot open (Win32 error " + std::to_string(GetLastError()) + ")";
    return std::nullopt;
  }
  // Vanilla writes its dummy contents and forces them before it locks.
  DWORD written = 0;
  OVERLAPPED write_at{};
  WriteFile(handle, kDummy, static_cast<DWORD>(kDummyBytes), &written, &write_at);
  FlushFileBuffers(handle);
  // `FileLock` is an exclusive lock over the whole file; Windows' equivalent
  // needs an OVERLAPPED, and FAIL_IMMEDIATELY is tryLock's non-blocking form.
  OVERLAPPED at{};
  if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                  MAXDWORD, &at)) {
    CloseHandle(handle);
    if (why != nullptr)
      *why = already_locked(lock_path);
    return std::nullopt;
  }
  return DirectoryLock(reinterpret_cast<std::intptr_t>(handle));
#else
  const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
  if (fd < 0) {
    if (why != nullptr)
      *why = lock_path.string() + ": " + std::strerror(errno);
    return std::nullopt;
  }
  // Vanilla's order is write, force, lock — so the file exists and is durable
  // even when the lock below is refused (a second server overwrites the same
  // three bytes, exactly as vanilla's create does).
  const ssize_t wrote = ::write(fd, kDummy, kDummyBytes);
  (void)wrote;
  ::fsync(fd);
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0; // to end of file: the whole-file lock FileChannel takes
  if (::fcntl(fd, F_SETLK, &lock) == -1) {
    const int err = errno;
    ::close(fd);
    if (why != nullptr)
      *why = (err == EACCES || err == EAGAIN) ? already_locked(lock_path)
                                              : lock_path.string() + ": " + std::strerror(err);
    return std::nullopt;
  }
  return DirectoryLock(fd);
#endif
}

bool DirectoryLock::is_locked(const std::filesystem::path& dir) {
  const std::filesystem::path lock_path = dir / kWorldLockFileName;
#if defined(_WIN32)
  HANDLE handle = CreateFileW(lock_path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    // Vanilla's probe answers true for AccessDeniedException and false for a
    // missing file; a sharing/lock violation is the locked case on Windows.
    return err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION ||
           err == ERROR_LOCK_VIOLATION;
  }
  OVERLAPPED at{};
  const bool locked = !LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0,
                                  MAXDWORD, MAXDWORD, &at);
  if (!locked) {
    OVERLAPPED unlock{};
    UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &unlock);
  }
  CloseHandle(handle);
  return locked;
#else
  // No O_CREAT: a missing lock file is vanilla's NoSuchFileException, i.e. an
  // unlocked level (the server creates it when it opens).
  const int fd = ::open(lock_path.c_str(), O_RDWR);
  if (fd < 0)
    return errno == EACCES; // vanilla: AccessDenied -> locked, missing -> not
  struct flock lock{};
  lock.l_type = F_WRLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;
  const bool locked = ::fcntl(fd, F_SETLK, &lock) == -1 && (errno == EACCES || errno == EAGAIN);
  if (!locked) {
    lock.l_type = F_UNLCK;
    ::fcntl(fd, F_SETLK, &lock);
  }
  ::close(fd);
  return locked;
#endif
}

DirectoryLock::~DirectoryLock() {
  close();
}

DirectoryLock::DirectoryLock(DirectoryLock&& other) noexcept
    : handle_(other.handle_), valid_(other.valid_) {
  other.handle_ = -1;
  other.valid_ = false;
}

DirectoryLock& DirectoryLock::operator=(DirectoryLock&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    valid_ = other.valid_;
    other.handle_ = -1;
    other.valid_ = false;
  }
  return *this;
}

void DirectoryLock::close() {
#if defined(_WIN32)
  if (handle_ != -1) {
    HANDLE handle = reinterpret_cast<HANDLE>(handle_);
    OVERLAPPED at{};
    UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &at);
    CloseHandle(handle);
  }
#else
  if (handle_ >= 0) {
    struct flock lock{};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    ::fcntl(static_cast<int>(handle_), F_SETLK, &lock);
    ::close(static_cast<int>(handle_));
  }
#endif
  handle_ = -1;
  valid_ = false;
}

} // namespace mc::world
