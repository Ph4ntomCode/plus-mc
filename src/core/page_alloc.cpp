// SPDX-License-Identifier: GPL-3.0-or-later
//
// The platform half of `core/page_alloc.h`: one anonymous, private, zero-filled
// run of pages and the release that returns it to the operating system at once.
//
// This lives in a .cpp rather than the header because the header is included
// almost everywhere (mob, chunk, light, nbt), and `<windows.h>` defines `near`,
// `far`, `min` and `max` as macros — `MobEnvironment near` and `Mob far` are
// ordinary identifiers in the world code. Keeping the platform header here
// means only this translation unit sees those macros.
#include "core/page_alloc.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(PLUSMC_PAGE_ALLOC_MAPPED)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace mc::core {

#if defined(PLUSMC_PAGE_ALLOC_MAPPED)

void* map_pages(std::size_t bytes) noexcept {
#if defined(_WIN32)
  // The commit is exactly `bytes`; the reservation rounds up to the allocation
  // granularity (64 KiB on x64). Only committed pages count against the working
  // set, so a 4 KiB cells array costs a 4 KiB working-set charge, exactly as the
  // mmap arm does on the POSIX builds.
  return ::VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

void unmap_pages(void* p, std::size_t bytes) noexcept {
#if defined(_WIN32)
  // MEM_RELEASE frees the whole reservation, so the length is not part of the
  // contract; the parameter stays for the mmap-shaped signature both sides use.
  (void)bytes;
  ::VirtualFree(p, 0, MEM_RELEASE);
#else
  ::munmap(p, bytes);
#endif
}

std::size_t system_page_bytes() noexcept {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return info.dwPageSize != 0 ? static_cast<std::size_t>(info.dwPageSize) : std::size_t{4096};
#else
  const long size = ::sysconf(_SC_PAGESIZE);
  return size > 0 ? static_cast<std::size_t>(size) : std::size_t{4096};
#endif
}

#endif

} // namespace mc::core
