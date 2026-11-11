// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deterministic stored-ZIP writer for `.pmcmod` packaging. No compression,
// no timestamps from the filesystem, no extra fields, sorted entry names: the
// same inputs produce the same archive bytes on every platform.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mc::modapi::detail {

struct ZipWriteEntry {
  std::string name;
  std::vector<uint8_t> data;
};

bool write_zip(const std::filesystem::path& path, std::vector<ZipWriteEntry> entries,
               std::string* error);

} // namespace mc::modapi::detail
