// SPDX-License-Identifier: GPL-3.0-or-later
//
// The handful of `Mth` helpers the terrain algebra is built from, written the
// way `Mth` writes them rather than the way they would be written from scratch.
// That is not pedantry: several of these differ from the "obvious" version in
// ways the fixtures can see.
//
//   * `clampedLerp` clamps with comparisons, not with a branch-free
//     `min`/`max`, so NaN passes through instead of being pinned to an end.
//   * `map` does not clamp at all — only `clampedMap` does.
//   * `quantize` floors a division and multiplies back, in double, before
//     converting to int.
//   * `wg_lfloor` is `(long)Math.floor(x)`, which saturates for out-of-range
//     doubles rather than wrapping.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace mc::world {

// `Mth.floor(double)` / `Mth.lfloor(double)`.
inline int32_t wg_floor(double value) {
  return static_cast<int32_t>(std::floor(value));
}
inline int64_t wg_lfloor(double value) {
  return static_cast<int64_t>(std::floor(value));
}

// `Math.floorDiv` / `Math.floorMod`, which round towards negative infinity.
inline int32_t wg_floor_div(int32_t a, int32_t b) {
  const int32_t quotient = a / b;
  return ((a % b) != 0 && ((a ^ b) < 0)) ? quotient - 1 : quotient;
}
inline int32_t wg_floor_mod(int32_t a, int32_t b) {
  return a - wg_floor_div(a, b) * b;
}

// `Mth.clamp`.
inline double wg_clamp(double value, double min, double max) {
  if (value < min)
    return min;
  return value > max ? max : value;
}

// `Mth.lerp` and friends. The argument order is vanilla's (delta first).
inline double wg_lerp(double delta, double start, double end) {
  return start + delta * (end - start);
}
inline double wg_lerp2(double d1, double d2, double x00, double x10, double x01, double x11) {
  return wg_lerp(d2, wg_lerp(d1, x00, x10), wg_lerp(d1, x01, x11));
}
inline double wg_lerp3(double d1, double d2, double d3, double x000, double x100, double x010,
                       double x110, double x001, double x101, double x011, double x111) {
  return wg_lerp(d3, wg_lerp2(d1, d2, x000, x100, x010, x110),
                 wg_lerp2(d1, d2, x001, x101, x011, x111));
}

inline double wg_smoothstep(double x) {
  return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

inline double wg_inverse_lerp(double value, double min, double max) {
  return (value - min) / (max - min);
}
inline double wg_clamped_lerp(double factor, double min, double max) {
  if (factor < 0.0)
    return min;
  if (factor > 1.0)
    return max;
  return wg_lerp(factor, min, max);
}
inline double wg_clamped_map(double value, double from_min, double from_max, double to_min,
                             double to_max) {
  return wg_clamped_lerp(wg_inverse_lerp(value, from_min, from_max), to_min, to_max);
}
inline double wg_map(double value, double from_min, double from_max, double to_min, double to_max) {
  return wg_lerp(wg_inverse_lerp(value, from_min, from_max), to_min, to_max);
}
inline int32_t wg_quantize(double value, int32_t resolution) {
  return wg_floor(value / static_cast<double>(resolution)) * resolution;
}

// Java's `Math.min` / `Math.max`, which let NaN through instead of picking an
// operand the way `std::min` would.
inline double wg_math_min(double a, double b) {
  if (a != a)
    return a;
  if (b != b)
    return b;
  return a < b ? a : b;
}
inline double wg_math_max(double a, double b) {
  if (a != a)
    return a;
  if (b != b)
    return b;
  return a > b ? a : b;
}

// ---- Java's `Math.sin` / `Math.cos`, bit for bit ---------------------------
//
// `Mth.SIN` is built with `(float)java.lang.Math.sin(i / 10430.378350470453)`,
// and a carver's tunnel angle goes through `Mth.sin`'s table. The platform
// libm's `sin` is *not* the same function: on macOS it rounds one table entry
// differently from the JVM's fdlibm, and that single float moves a tunnel by
// enough to miss a chunk (the carver fixtures catch it). Java's `Math.sin` is
// `StrictMath.sin` on this JDK, i.e. fdlibm, so the port below is fdlibm's
// `Sin.compute`/`Cos.compute` with its medium-size argument reduction — the
// only range the carver uses (angles are below 2*pi).
namespace wg_fdlibm {

inline int32_t high_word(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<int32_t>(bits >> 32);
}

inline double with_high_word(int32_t high, int32_t low) {
  const uint64_t bits =
      (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32) | static_cast<uint32_t>(low);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

constexpr int32_t kExpSignifBits = 0x7fffffff;
constexpr int32_t kExpBits = 0x7ff00000;

// `Sin.__kernel_sin`.
inline double kernel_sin(double x, double y, int iy) {
  constexpr double kS1 = -0x1.5555555555549p-3;
  constexpr double kS2 = 0x1.111111110f8a6p-7;
  constexpr double kS3 = -0x1.a01a019c161d5p-13;
  constexpr double kS4 = 0x1.71de357b1fe7dp-19;
  constexpr double kS5 = -0x1.ae5e68a2b9cebp-26;
  constexpr double kS6 = 0x1.5d93a5acfd57cp-33;
  const int32_t ix = high_word(x) & kExpSignifBits;
  if (ix < 0x3e400000 && static_cast<int32_t>(x) == 0)
    return x;
  const double z = x * x;
  const double v = z * x;
  const double r = kS2 + z * (kS3 + z * (kS4 + z * (kS5 + z * kS6)));
  if (iy == 0)
    return x + v * (kS1 + z * r);
  return x - ((z * (0.5 * y - v * r) - y) - v * kS1);
}

// `Cos.__kernel_cos`.
inline double kernel_cos(double x, double y) {
  constexpr double kC1 = 0x1.555555555554cp-5;
  constexpr double kC2 = -0x1.6c16c16c15177p-10;
  constexpr double kC3 = 0x1.a01a019cb159p-16;
  constexpr double kC4 = -0x1.27e4f809c52adp-22;
  constexpr double kC5 = 0x1.1ee9ebdb4b1c4p-29;
  constexpr double kC6 = -0x1.8fae9be8838d4p-37;
  const int32_t ix = high_word(x) & kExpSignifBits;
  if (ix < 0x3e400000 && static_cast<int32_t>(x) == 0)
    return 1.0;
  const double z = x * x;
  const double r = z * (kC1 + z * (kC2 + z * (kC3 + z * (kC4 + z * (kC5 + z * kC6)))));
  if (ix < 0x3fd33333)
    return 1.0 - (0.5 * z - (z * r - x * y));
  const double qx = ix > 0x3fe90000 ? 0.28125 : with_high_word(ix - 0x00200000, 0);
  const double hz = 0.5 * z - qx;
  const double a = 1.0 - qx;
  return a - (hz - (z * r - x * y));
}

// `RemPio2.__ieee754_rem_pio2`, medium path (`|x| <= 2^19 * pi/2`).
inline int rem_pio2_medium(double x, double& y0, double& y1) {
  constexpr double kInvpio2 = 0x1.45f306dc9c883p-1;
  constexpr double kPio2_1 = 0x1.921fb544p0;
  constexpr double kPio2_1t = 0x1.0b4611a626331p-34;
  constexpr double kPio2_2 = 0x1.0b4611a6p-34;
  constexpr double kPio2_2t = 0x1.3198a2e037073p-69;
  constexpr double kPio2_3 = 0x1.3198a2ep-69;
  constexpr double kPio2_3t = 0x1.b839a252049c1p-104;
  constexpr int32_t kNpio2Hw[32] = {
      0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C, 0x4025FDBB,
      0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C, 0x40346B9C, 0x4035FDBB,
      0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A, 0x403DD85A, 0x403F6A7A, 0x40407E4C,
      0x4041475C, 0x4042106C, 0x4042D97C, 0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB,
      0x4046C6CB, 0x40478FDB, 0x404858EB, 0x404921FB,
  };
  const int32_t hx = high_word(x);
  const int32_t ix = hx & kExpSignifBits;
  double t = std::fabs(x);
  const int32_t n = static_cast<int32_t>(t * kInvpio2 + 0.5);
  const double fn = static_cast<double>(n);
  double r = t - fn * kPio2_1;
  double w = fn * kPio2_1t;
  if (n < 32 && ix != kNpio2Hw[n - 1]) {
    y0 = r - w;
  } else {
    const int32_t j = ix >> 20;
    y0 = r - w;
    int32_t i = j - ((high_word(y0) >> 20) & 0x7ff);
    if (i > 16) {
      t = r;
      w = fn * kPio2_2;
      r = t - w;
      w = fn * kPio2_2t - ((t - r) - w);
      y0 = r - w;
      i = j - ((high_word(y0) >> 20) & 0x7ff);
      if (i > 49) {
        t = r;
        w = fn * kPio2_3;
        r = t - w;
        w = fn * kPio2_3t - ((t - r) - w);
        y0 = r - w;
      }
    }
  }
  y1 = (r - y0) - w;
  if (hx < 0) {
    y0 = -y0;
    y1 = -y1;
    return -n;
  }
  return n;
}

} // namespace wg_fdlibm

// `Math.sin(double)` for the range the carver uses. Outside the medium
// reduction the platform libm is the only fallback; nothing on this path
// reaches it (angles are below 2*pi).
inline double wg_java_sin(double x) {
  const int32_t ix = wg_fdlibm::high_word(x) & wg_fdlibm::kExpSignifBits;
  if (ix <= 0x3fe921fb)
    return wg_fdlibm::kernel_sin(x, 0.0, 0);
  if (ix >= wg_fdlibm::kExpBits)
    return x - x;
  if (ix > 0x413921fb)
    return std::sin(x);
  double y0 = 0.0;
  double y1 = 0.0;
  const int n = wg_fdlibm::rem_pio2_medium(x, y0, y1);
  switch (n & 3) {
  case 0:
    return wg_fdlibm::kernel_sin(y0, y1, 1);
  case 1:
    return wg_fdlibm::kernel_cos(y0, y1);
  case 2:
    return -wg_fdlibm::kernel_sin(y0, y1, 1);
  default:
    return -wg_fdlibm::kernel_cos(y0, y1);
  }
}

inline double wg_java_cos(double x) {
  const int32_t ix = wg_fdlibm::high_word(x) & wg_fdlibm::kExpSignifBits;
  if (ix <= 0x3fe921fb)
    return wg_fdlibm::kernel_cos(x, 0.0);
  if (ix >= wg_fdlibm::kExpBits)
    return x - x;
  if (ix > 0x413921fb)
    return std::cos(x);
  double y0 = 0.0;
  double y1 = 0.0;
  const int n = wg_fdlibm::rem_pio2_medium(x, y0, y1);
  switch (n & 3) {
  case 0:
    return wg_fdlibm::kernel_cos(y0, y1);
  case 1:
    return -wg_fdlibm::kernel_sin(y0, y1, 1);
  case 2:
    return -wg_fdlibm::kernel_cos(y0, y1);
  default:
    return wg_fdlibm::kernel_sin(y0, y1, 1);
  }
}

} // namespace mc::world
