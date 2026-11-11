// SPDX-License-Identifier: GPL-3.0-or-later

#include "modapi/zip_archive.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <unordered_set>

#include <zlib.h>

namespace mc::modapi::detail {
namespace {

constexpr uint64_t kMaxArchiveBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaxEntryBytes = 64ull * 1024 * 1024;
constexpr uint64_t kMaxTotalBytes = 256ull * 1024 * 1024;
constexpr std::size_t kMaxEntries = 4096;

uint16_t rd16(const std::vector<uint8_t>& b, std::size_t off) {
  return static_cast<uint16_t>(static_cast<uint16_t>(b[off]) |
                               (static_cast<uint16_t>(b[off + 1]) << 8));
}

uint32_t rd32(const std::vector<uint8_t>& b, std::size_t off) {
  return static_cast<uint32_t>(
      static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
      (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24));
}

std::string fail(std::string* error, std::string message) {
  if (error != nullptr)
    *error = std::move(message);
  return {};
}

bool local_data_range(const std::vector<uint8_t>& data, const ZipEntry& entry, std::size_t* start,
                      std::string* error) {
  const uint64_t local = entry.local_header_offset;
  if (local + 30 > data.size() || rd32(data, static_cast<std::size_t>(local)) != 0x04034b50u) {
    *error = "zip: bad local header for " + entry.name;
    return false;
  }
  const uint16_t name_len = rd16(data, static_cast<std::size_t>(local) + 26);
  const uint16_t extra_len = rd16(data, static_cast<std::size_t>(local) + 28);
  const uint64_t begin = local + 30 + name_len + extra_len;
  if (begin + entry.compressed_size > data.size()) {
    *error = "zip: truncated entry " + entry.name;
    return false;
  }
  *start = static_cast<std::size_t>(begin);
  return true;
}

} // namespace

bool safe_archive_path(const std::string& name, std::string* error) {
  if (name.empty() || name.size() > 1024) {
    *error = "zip: entry path is empty or too long";
    return false;
  }
  if (name.front() == '/' || name.front() == '\\') {
    *error = "zip: absolute entry path '" + name + "'";
    return false;
  }
  if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) != 0 &&
      name[1] == ':') {
    *error = "zip: drive-qualified entry path '" + name + "'";
    return false;
  }
  std::size_t segment_start = 0;
  for (std::size_t i = 0; i <= name.size(); ++i) {
    if (i == name.size() || name[i] == '/') {
      const std::string segment = name.substr(segment_start, i - segment_start);
      if (segment == "..") {
        *error = "zip: path traversal in '" + name + "'";
        return false;
      }
      if (segment == ".") {
        *error = "zip: '.' segment in '" + name + "'";
        return false;
      }
      segment_start = i + 1;
    }
  }
  for (const unsigned char c : name) {
    if (c == '\\') {
      *error = "zip: backslash in entry path '" + name + "'";
      return false;
    }
    if (c == 0 || c < 0x20 || c == 0x7f) {
      *error = "zip: control character in entry path '" + name + "'";
      return false;
    }
  }
  return true;
}

bool ZipArchive::open(const std::filesystem::path& path, std::string* error) {
  data_.clear();
  entries_.clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    fail(error, "cannot read " + path.string());
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff length = in.tellg();
  if (length < 0 || static_cast<uint64_t>(length) > kMaxArchiveBytes) {
    fail(error, "archive is too large");
    return false;
  }
  in.seekg(0, std::ios::beg);
  data_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (data_.size() < 22) {
    fail(error, "archive is too short");
    return false;
  }

  // End Of Central Directory, searched backwards within the maximum comment
  // length. The comment itself is not used.
  const std::size_t floor = data_.size() > 22 + 65535 ? data_.size() - 22 - 65535 : 0;
  std::size_t eocd = data_.size();
  for (std::size_t i = data_.size() - 22;; --i) {
    if (rd32(data_, i) == 0x06054b50u) {
      eocd = i;
      break;
    }
    if (i == floor)
      break;
  }
  if (eocd == data_.size()) {
    fail(error, "archive has no central directory");
    return false;
  }
  if (eocd + 22 > data_.size()) {
    fail(error, "truncated central directory");
    return false;
  }
  if (rd16(data_, eocd + 4) != 0 || rd16(data_, eocd + 6) != 0) {
    fail(error, "multi-disk archives are not supported");
    return false;
  }
  const uint16_t count = rd16(data_, eocd + 10);
  const uint32_t cd_size = rd32(data_, eocd + 12);
  const uint32_t cd_offset = rd32(data_, eocd + 16);
  if (count == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
    fail(error, "ZIP64 archives are not supported");
    return false;
  }
  if (count > kMaxEntries) {
    fail(error, "archive has too many entries");
    return false;
  }
  if (static_cast<uint64_t>(cd_offset) + cd_size > data_.size()) {
    fail(error, "central directory is out of bounds");
    return false;
  }

  std::size_t pos = cd_offset;
  uint64_t total_uncompressed = 0;
  std::unordered_set<std::string> names;
  for (uint16_t n = 0; n < count; ++n) {
    if (pos + 46 > data_.size() || rd32(data_, pos) != 0x02014b50u) {
      fail(error, "bad central directory entry");
      return false;
    }
    ZipEntry entry;
    entry.flags = rd16(data_, pos + 8);
    entry.method = rd16(data_, pos + 10);
    entry.crc = rd32(data_, pos + 16);
    const uint32_t csize = rd32(data_, pos + 20);
    const uint32_t usize = rd32(data_, pos + 24);
    const uint16_t name_len = rd16(data_, pos + 28);
    const uint16_t extra_len = rd16(data_, pos + 30);
    const uint16_t comment_len = rd16(data_, pos + 32);
    const uint32_t external_attr = rd32(data_, pos + 38);
    entry.local_header_offset = rd32(data_, pos + 42);

    if ((entry.flags & 1u) != 0) {
      fail(error, "encrypted entries are not supported");
      return false;
    }
    if (entry.method != 0 && entry.method != 8) {
      fail(error, "unsupported compression method");
      return false;
    }
    if (csize == 0xffffffffu || usize == 0xffffffffu) {
      fail(error, "ZIP64 entries are not supported");
      return false;
    }
    // Unix symlink entries have S_IFLNK in the high half of external_attr.
    if (((external_attr >> 16) & 0xf000u) == 0xa000u) {
      fail(error, "symlink entries are not supported");
      return false;
    }
    if (pos + 46 + name_len > data_.size()) {
      fail(error, "truncated central directory name");
      return false;
    }
    entry.name.assign(reinterpret_cast<const char*>(&data_[pos + 46]), name_len);
    if (!safe_archive_path(entry.name, error))
      return false;
    if (!names.insert(entry.name).second) {
      fail(error, "duplicate archive entry '" + entry.name + "'");
      return false;
    }
    entry.compressed_size = csize;
    entry.uncompressed_size = usize;
    if (entry.uncompressed_size > kMaxEntryBytes) {
      fail(error, "archive entry is too large: " + entry.name);
      return false;
    }
    total_uncompressed += entry.uncompressed_size;
    if (total_uncompressed > kMaxTotalBytes) {
      fail(error, "archive expands beyond its total limit");
      return false;
    }

    std::size_t start = 0;
    std::string local_error;
    if (!local_data_range(data_, entry, &start, &local_error)) {
      fail(error, local_error);
      return false;
    }
    const std::size_t local = static_cast<std::size_t>(entry.local_header_offset);
    const uint16_t local_name_len = rd16(data_, local + 26);
    if (local_name_len != name_len ||
        std::string(reinterpret_cast<const char*>(&data_[local + 30]), name_len) != entry.name) {
      fail(error, "local and central names disagree for '" + entry.name + "'");
      return false;
    }
    entries_.push_back(std::move(entry));

    pos += 46u + name_len + extra_len + comment_len;
    if (pos > data_.size()) {
      fail(error, "central directory entry runs past the archive");
      return false;
    }
  }
  return true;
}

const ZipEntry* ZipArchive::find(const std::string& name) const {
  for (const ZipEntry& entry : entries_)
    if (entry.name == name)
      return &entry;
  return nullptr;
}

bool ZipArchive::read(const ZipEntry& entry, std::vector<uint8_t>* out, std::string* error) const {
  std::size_t start = 0;
  if (!local_data_range(data_, entry, &start, error))
    return false;
  if (entry.method == 0) {
    if (entry.compressed_size != entry.uncompressed_size) {
      fail(error, "stored entry has mismatched sizes: " + entry.name);
      return false;
    }
    out->assign(data_.begin() + static_cast<std::ptrdiff_t>(start),
                data_.begin() + static_cast<std::ptrdiff_t>(start + entry.compressed_size));
    return true;
  }
  if (entry.uncompressed_size > std::numeric_limits<uInt>::max() ||
      entry.compressed_size > std::numeric_limits<uInt>::max()) {
    fail(error, "entry is too large to inflate: " + entry.name);
    return false;
  }
  z_stream zs{};
  if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
    fail(error, "cannot start inflate for " + entry.name);
    return false;
  }
  std::vector<uint8_t> buffer(entry.uncompressed_size == 0 ? 1 : entry.uncompressed_size);
  zs.next_in = const_cast<Bytef*>(&data_[start]);
  zs.avail_in = static_cast<uInt>(entry.compressed_size);
  zs.next_out = buffer.data();
  zs.avail_out = static_cast<uInt>(buffer.size());
  const int rc = inflate(&zs, Z_FINISH);
  const uLong produced = zs.total_out;
  inflateEnd(&zs);
  if (rc != Z_STREAM_END || produced != entry.uncompressed_size) {
    fail(error, "inflate failed for " + entry.name);
    return false;
  }
  buffer.resize(static_cast<std::size_t>(produced));
  *out = std::move(buffer);
  return true;
}

} // namespace mc::modapi::detail
