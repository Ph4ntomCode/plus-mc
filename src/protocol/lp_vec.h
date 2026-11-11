// SPDX-License-Identifier: GPL-3.0-or-later
//
// LpVec3 (net.minecraft.network.LpVec3): a compressed Vec3 used by 26.2 for
// entity movement on the wire. The low three bits of the first byte hold the
// 2-bit scale plus a continuation flag (bit 2); when the flag is set the
// remaining scale bits follow as a VarInt. The three components are packed at
// 15 bits each (offsets 3/18/33), capped at 32766.
//
// A near-zero vector — every component below ABS_MIN_VALUE (3.05e-5) — is a
// single 0x00 byte, which is what vanilla sends for an entity's initial
// zero velocity (Add Entity, item drops, etc.).
//
// Layout mirrors LpVec3.write exactly, so a vanilla 26.2 client decodes our
// bytes: marker byte, marker byte, big-endian u32, optional scale VarInt.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "net/byte_buffer.h"

namespace mc::protocol {

constexpr double kLpVecAbsMin = 3.051944088384301e-5;
constexpr double kLpVecAbsMax = 1.7179869183e10;
constexpr double kLpVecQuantMax = 32766.0;

inline void write_lp_vec3(mc::net::Writer& w, double x, double y, double z) {
  auto sanitize = [](double v) {
    if (std::isnan(v))
      return 0.0;
    return std::clamp(v, -kLpVecAbsMax, kLpVecAbsMax);
  };
  x = sanitize(x);
  y = sanitize(y);
  z = sanitize(z);

  const double len = std::max(std::abs(x), std::max(std::abs(y), std::abs(z)));
  if (len < kLpVecAbsMin) {
    w.u8(0);
    return;
  }

  const int64_t scale = static_cast<int64_t>(std::ceil(len));
  const bool partial = (scale & 3) != scale;
  const int64_t markers = partial ? ((scale & 3) | 4) : scale;

  // Java Math.round(x) == floor(x + 0.5); inputs here are in [0, 32766].
  auto pack = [](double v) -> int64_t {
    return static_cast<int64_t>(std::floor((v * 0.5 + 0.5) * kLpVecQuantMax + 0.5));
  };

  const int64_t buffer = markers | (pack(x / static_cast<double>(scale)) << 3) |
                         (pack(y / static_cast<double>(scale)) << 18) |
                         (pack(z / static_cast<double>(scale)) << 33);
  w.u8(static_cast<uint8_t>(buffer));
  w.u8(static_cast<uint8_t>(buffer >> 8));
  w.u32(static_cast<uint32_t>(buffer >> 16));
  if (partial)
    w.varint(static_cast<uint32_t>(scale >> 2));
}

// `LpVec3.read`, for the one serverbound packet that carries one: the interaction
// location of ServerboundInteractPacket (0x1a). It is the writer's inverse — one
// 0x00 byte is the zero vector, otherwise the two marker bytes and the big-endian
// u32 reassemble the buffer, the scale comes from the first byte's low two bits
// (with the continuation varint's high bits), and each 15-bit component is
// unpacked back to [-1, 1]. Two details are vanilla's rather than a tidy
// inverse: `unpack` masks with 0x7FFF and then *clamps* to 32766, and the
// divisor is 32766 rather than the mask's 32767.
inline bool read_lp_vec3(mc::net::Reader& r, double* x, double* y, double* z) {
  *x = 0.0;
  *y = 0.0;
  *z = 0.0;
  uint8_t lowest = 0;
  if (!r.u8(&lowest))
    return false;
  if (lowest == 0)
    return true;
  uint8_t middle = 0;
  uint32_t highest = 0;
  if (!r.u8(&middle) || !r.u32(&highest))
    return false;
  const int64_t buffer = (static_cast<int64_t>(highest) << 16) |
                         (static_cast<int64_t>(middle) << 8) | static_cast<int64_t>(lowest);
  int64_t scale = lowest & 3;
  if ((lowest & 4) != 0) {
    uint32_t continuation = 0;
    if (!r.varint(&continuation))
      return false;
    scale |= static_cast<int64_t>(continuation) << 2;
  }
  const auto unpack = [](int64_t value) {
    const double quantized = static_cast<double>(std::min<int64_t>(value & 0x7FFF, 32766));
    return quantized * 2.0 / 32766.0 - 1.0;
  };
  *x = unpack(buffer >> 3) * static_cast<double>(scale);
  *y = unpack(buffer >> 18) * static_cast<double>(scale);
  *z = unpack(buffer >> 33) * static_cast<double>(scale);
  return true;
}

} // namespace mc::protocol
