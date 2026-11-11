// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/byte_buffer.h"

namespace mc::net {

int encode_varint(uint32_t value, std::vector<uint8_t>& out) {
  int count = 0;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    out.push_back(byte);
    ++count;
  } while (value != 0);
  return count;
}

int encode_varlong(uint64_t value, std::vector<uint8_t>& out) {
  int count = 0;
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7F);
    value >>= 7;
    if (value != 0)
      byte |= 0x80;
    out.push_back(byte);
    ++count;
  } while (value != 0);
  return count;
}

bool decode_varint(const std::vector<uint8_t>& data, size_t* pos, uint32_t* out) {
  // Transactional: on any failure *pos is left untouched (documented in the
  // header), so callers can recover instead of desyncing on malformed input.
  const size_t start = *pos;
  uint32_t value = 0;
  for (int i = 0; i < 5; ++i) {
    if (*pos >= data.size()) {
      *pos = start;
      return false;
    }
    uint8_t byte = data[(*pos)++];
    // Values must fit 32 bits: the 5th byte may only use its low 4 bits
    // (sign extension of negative VarInts tops out at 0x0F). Anything above
    // that means the stream is corrupt or hostile.
    if (i == 4 && (byte & 0xF0) != 0) {
      *pos = start;
      return false;
    }
    value |= static_cast<uint32_t>(byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0) {
      *out = value;
      return true;
    }
  }
  *pos = start;
  return false; // more than 5 bytes: invalid VarInt
}

} // namespace mc::net
