// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small read-only ZIP reader for `.pmcmod` verification. It deliberately
// supports only the part of the format a package needs: stored and raw-deflate
// entries, a single disk, and no ZIP64. Every bound is checked before bytes are
// handed back so a hostile archive cannot make the verifier allocate first and
// ask questions later.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::modapi::detail {

struct ZipEntry {
  std::string name;
  uint16_t method = 0;
  uint32_t crc = 0;
  uint64_t compressed_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t local_header_offset = 0;
  uint16_t flags = 0;
};

class ZipArchive {
public:
  bool open(const std::filesystem::path& path, std::string* error);
  const std::vector<ZipEntry>& entries() const {
    return entries_;
  }
  const ZipEntry* find(const std::string& name) const;
  bool read(const ZipEntry& entry, std::vector<uint8_t>* out, std::string* error) const;
  uint64_t size() const {
    return data_.size();
  }

private:
  std::vector<uint8_t> data_;
  std::vector<ZipEntry> entries_;
};

// A package entry path is a forward-slash relative path. Reject absolute paths,
// drive letters, backslashes, NULs, control characters and any `..` segment.
bool safe_archive_path(const std::string& name, std::string* error);

} // namespace mc::modapi::detail
