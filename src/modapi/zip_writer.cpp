// SPDX-License-Identifier: GPL-3.0-or-later

#include "modapi/zip_writer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>

#include <zlib.h>

#include "modapi/zip_archive.h"

namespace mc::modapi::detail {
namespace {

constexpr uint16_t kVersion = 20;
constexpr uint16_t kDosDate = 0x0021; // 1980-01-01, the ZIP epoch
constexpr uint16_t kDosTime = 0;
constexpr std::size_t kMaxEntries = 65535;

void put16(std::vector<uint8_t>* out, uint16_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void put32(std::vector<uint8_t>* out, uint32_t value) {
  out->push_back(static_cast<uint8_t>(value & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

uint32_t crc32_of(const std::vector<uint8_t>& bytes) {
  uLong crc = crc32(0L, Z_NULL, 0);
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t chunk = std::min<std::size_t>(bytes.size() - offset, 1u << 20);
    crc = crc32(crc, bytes.data() + offset, static_cast<uInt>(chunk));
    offset += chunk;
  }
  return static_cast<uint32_t>(crc);
}

struct PlacedEntry {
  std::string name;
  std::vector<uint8_t> data;
  uint32_t crc = 0;
  uint32_t offset = 0;
};

} // namespace

bool write_zip(const std::filesystem::path& path, std::vector<ZipWriteEntry> entries,
               std::string* error) {
  if (entries.size() > kMaxEntries) {
    *error = "zip: too many entries";
    return false;
  }
  std::sort(
      entries.begin(), entries.end(),
      [](const ZipWriteEntry& left, const ZipWriteEntry& right) { return left.name < right.name; });
  std::set<std::string> names;
  uint64_t total = 0;
  for (const ZipWriteEntry& entry : entries) {
    if (!safe_archive_path(entry.name, error))
      return false;
    if (!names.insert(entry.name).second) {
      *error = "zip: duplicate entry '" + entry.name + "'";
      return false;
    }
    if (entry.data.size() > std::numeric_limits<uint32_t>::max()) {
      *error = "zip: entry is too large: " + entry.name;
      return false;
    }
    total += entry.data.size();
    if (total > std::numeric_limits<uint32_t>::max()) {
      *error = "zip: archive is too large";
      return false;
    }
  }

  std::vector<uint8_t> output;
  std::vector<PlacedEntry> placed;
  for (ZipWriteEntry& entry : entries) {
    PlacedEntry item;
    item.name = std::move(entry.name);
    item.data = std::move(entry.data);
    item.crc = crc32_of(item.data);
    item.offset = static_cast<uint32_t>(output.size());

    put32(&output, 0x04034b50u);
    put16(&output, kVersion);
    put16(&output, 0); // no flags: stored bytes, no data descriptor
    put16(&output, 0); // stored
    put16(&output, kDosTime);
    put16(&output, kDosDate);
    put32(&output, item.crc);
    put32(&output, static_cast<uint32_t>(item.data.size()));
    put32(&output, static_cast<uint32_t>(item.data.size()));
    put16(&output, static_cast<uint16_t>(item.name.size()));
    put16(&output, 0); // extra
    output.insert(output.end(), item.name.begin(), item.name.end());
    output.insert(output.end(), item.data.begin(), item.data.end());
    placed.push_back(std::move(item));
  }

  const uint32_t central_offset = static_cast<uint32_t>(output.size());
  for (const PlacedEntry& item : placed) {
    put32(&output, 0x02014b50u);
    put16(&output, kVersion); // made by
    put16(&output, kVersion); // needed
    put16(&output, 0);
    put16(&output, 0); // stored
    put16(&output, kDosTime);
    put16(&output, kDosDate);
    put32(&output, item.crc);
    put32(&output, static_cast<uint32_t>(item.data.size()));
    put32(&output, static_cast<uint32_t>(item.data.size()));
    put16(&output, static_cast<uint16_t>(item.name.size()));
    put16(&output, 0); // extra
    put16(&output, 0); // comment
    put16(&output, 0); // disk
    put16(&output, 0); // internal attrs
    put32(&output, 0); // external attrs
    put32(&output, item.offset);
    output.insert(output.end(), item.name.begin(), item.name.end());
  }
  const uint32_t central_size = static_cast<uint32_t>(output.size()) - central_offset;

  put32(&output, 0x06054b50u);
  put16(&output, 0);
  put16(&output, 0);
  put16(&output, static_cast<uint16_t>(placed.size()));
  put16(&output, static_cast<uint16_t>(placed.size()));
  put32(&output, central_size);
  put32(&output, central_offset);
  put16(&output, 0); // comment

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    *error = "cannot write " + path.string();
    return false;
  }
  out.write(reinterpret_cast<const char*>(output.data()),
            static_cast<std::streamsize>(output.size()));
  if (!out) {
    *error = "write failed: " + path.string();
    return false;
  }
  return true;
}

} // namespace mc::modapi::detail
