// SPDX-License-Identifier: GPL-3.0-or-later
//
// See `worldgen_noise.h` for why each piece exists and which vanilla class it
// mirrors. Two constants in here are worth spelling out because they look
// arbitrary and are not:
//
//   * `ImprovedNoise`'s `1.0E-7f` is a *float* literal cast to double
//     (`1.0000000116860974e-07`), matching 26.2's source. Writing `1.0e-7`
//     would be a different, slightly smaller number.
//   * `PerlinNoise.wrap` uses `3.3554432E7` (2^25) and `Mth.lfloor`, so it
//     folds a coordinate into a 2^25-wide cell around zero — this is what
//     keeps far-from-origin terrain from drifting.

#include "world/worldgen_noise.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

#include "world/worldgen_math.h"

namespace mc::world {
namespace {

// `SimplexNoise.GRADIENT`. Note the last four entries repeat earlier
// directions — that is the real table, not a typo.
constexpr int32_t kGradient[16][3] = {
    {1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1},
    {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, -1, -1},
};

inline double gradient_dot(int32_t hash, double x, double y, double z) {
  const int32_t* g = kGradient[hash & 0xF];
  return static_cast<double>(g[0]) * x + static_cast<double>(g[1]) * y +
         static_cast<double>(g[2]) * z;
}

double expected_deviation(int32_t octave_span) {
  return 0.1 * (1.0 + 1.0 / static_cast<double>(octave_span + 1));
}

// A null amplitude list can only mean "no octaves" — every loop over `count`
// below indexes it, and callers that pass null with a positive count used to
// dereference it. Reading zero keeps the octave count (which is what shapes the
// random stream) and marks every octave absent, the same answer an all-zero
// list gives.
inline double amplitude_at(const double* amplitudes, int index) {
  return amplitudes != nullptr ? amplitudes[index] : 0.0;
}

// The two random-source flavours share one implementation; these overloads
// pick the right `WgPerlinNoise` constructor without making the template
// instantiate calls that do not exist for the other source type.
WgPerlinNoise make_perlin(WgRandom& random, int32_t first_octave, const double* amplitudes,
                          int count, bool use_new_initialization) {
  return WgPerlinNoise(random, first_octave, amplitudes, count, use_new_initialization);
}
WgPerlinNoise make_perlin(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes,
                          int count, bool use_new_initialization) {
  return WgPerlinNoise(random, first_octave, amplitudes, count, use_new_initialization);
}
WgPerlinNoise make_legacy_perlin(WgRandom& random, int32_t first_octave, const double* amplitudes,
                                 int count) {
  return WgPerlinNoise(random, first_octave, amplitudes, count, false);
}
WgPerlinNoise make_legacy_perlin(WgLegacyRandom& random, int32_t first_octave,
                                 const double* amplitudes, int count) {
  return WgPerlinNoise(random, first_octave, amplitudes, count);
}

} // namespace

void wg_skip_octave(WgRandom& random) {
  random.consume_count(262);
}

void WgImprovedNoise::init(WgRandom& random) {
  init_from(random);
}

void WgImprovedNoise::init(WgLegacyRandom& random) {
  init_from(random);
}

template <typename Random> void WgImprovedNoise::init_from(Random& random) {
  xo_ = random.next_double() * 256.0;
  yo_ = random.next_double() * 256.0;
  zo_ = random.next_double() * 256.0;
  for (int i = 0; i < 256; ++i) {
    p_[i] = static_cast<uint8_t>(i);
  }
  for (int i = 0; i < 256; ++i) {
    const int offset = random.next_int(256 - i);
    const uint8_t tmp = p_[i];
    p_[i] = p_[i + offset];
    p_[i + offset] = tmp;
  }
}

double WgImprovedNoise::grad_dot(int hash, double x, double y, double z) {
  return gradient_dot(hash, x, y, z);
}

double WgImprovedNoise::noise(double _x, double _y, double _z, double y_scale,
                              double y_fudge) const {
  const double x = _x + xo_;
  const double y = _y + yo_;
  const double z = _z + zo_;
  const int32_t xf = wg_floor(x);
  const int32_t yf = wg_floor(y);
  const int32_t zf = wg_floor(z);
  const double xr = x - static_cast<double>(xf);
  const double yr = y - static_cast<double>(yf);
  const double zr = z - static_cast<double>(zf);
  double yr_fudge = 0.0;
  if (y_scale != 0.0) {
    const double fudge_limit = (y_fudge >= 0.0 && y_fudge < yr) ? y_fudge : yr;
    yr_fudge =
        static_cast<double>(wg_lfloor(fudge_limit / y_scale + static_cast<double>(1.0e-7f))) *
        y_scale;
  }
  return sample_and_lerp(xf, yf, zf, xr, yr - yr_fudge, zr, yr);
}

double WgImprovedNoise::sample_and_lerp(int x, int y, int z, double xr, double yr, double zr,
                                        double yr_original) const {
  const int x0 = perm(x);
  const int x1 = perm(x + 1);
  const int xy00 = perm(x0 + y);
  const int xy01 = perm(x0 + y + 1);
  const int xy10 = perm(x1 + y);
  const int xy11 = perm(x1 + y + 1);
  const double d000 = grad_dot(perm(xy00 + z), xr, yr, zr);
  const double d100 = grad_dot(perm(xy10 + z), xr - 1.0, yr, zr);
  const double d010 = grad_dot(perm(xy01 + z), xr, yr - 1.0, zr);
  const double d110 = grad_dot(perm(xy11 + z), xr - 1.0, yr - 1.0, zr);
  const double d001 = grad_dot(perm(xy00 + z + 1), xr, yr, zr - 1.0);
  const double d101 = grad_dot(perm(xy10 + z + 1), xr - 1.0, yr, zr - 1.0);
  const double d011 = grad_dot(perm(xy01 + z + 1), xr, yr - 1.0, zr - 1.0);
  const double d111 = grad_dot(perm(xy11 + z + 1), xr - 1.0, yr - 1.0, zr - 1.0);
  // The y alpha is the *unfudged* fractional part; that is the whole reason
  // `yr_original` is threaded through.
  const double x_alpha = wg_smoothstep(xr);
  const double y_alpha = wg_smoothstep(yr_original);
  const double z_alpha = wg_smoothstep(zr);
  return wg_lerp3(x_alpha, y_alpha, z_alpha, d000, d100, d010, d110, d001, d101, d011, d111);
}

WgPerlinNoise::WgPerlinNoise(WgRandom& random, int32_t first_octave, const double* amplitudes,
                             int count, bool use_new_initialization) {
  count_ = count;
  if (amplitudes != nullptr && count > 0)
    amplitudes_.assign(amplitudes, amplitudes + count);
  levels_.resize(static_cast<size_t>(count));
  present_.assign(static_cast<size_t>(count), 0);
  const int32_t zero_octave_index = -first_octave;
  if (use_new_initialization) {
    WgPositionalFactory positional = random.fork_positional();
    for (int i = 0; i < count; ++i) {
      if (amplitude_at(amplitudes, i) == 0.0)
        continue;
      const int32_t octave = first_octave + i;
      WgRandom octave_random = positional.from_hash_of("octave_" + std::to_string(octave));
      levels_[static_cast<size_t>(i)].init(octave_random);
      present_[static_cast<size_t>(i)] = 1;
    }
  } else {
    // The legacy construction: the octave at `-firstOctave` is created first,
    // then the lower ones in descending order. An octave whose amplitude is
    // zero still costs 262 `nextLong`s.
    WgImprovedNoise zero_octave;
    zero_octave.init(random);
    if (zero_octave_index >= 0 && zero_octave_index < count) {
      levels_[static_cast<size_t>(zero_octave_index)] = zero_octave;
      if (amplitude_at(amplitudes, zero_octave_index) != 0.0) {
        present_[static_cast<size_t>(zero_octave_index)] = 1;
      }
    }
    for (int i = zero_octave_index - 1; i >= 0; --i) {
      if (i < count) {
        if (amplitude_at(amplitudes, i) != 0.0) {
          levels_[static_cast<size_t>(i)].init(random);
          present_[static_cast<size_t>(i)] = 1;
          continue;
        }
        wg_skip_octave(random);
        continue;
      }
      wg_skip_octave(random);
    }
  }
  lowest_freq_input_factor_ = std::pow(2.0, -zero_octave_index);
  lowest_freq_value_factor_ = std::pow(2.0, count - 1) / (std::pow(2.0, count) - 1.0);
}

WgPerlinNoise::WgPerlinNoise(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes,
                             int count)
    : WgPerlinNoise(random, first_octave, amplitudes, count,
                    /*use_new_initialization=*/false) {}

WgPerlinNoise::WgPerlinNoise(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes,
                             int count, bool use_new_initialization) {
  count_ = count;
  if (amplitudes != nullptr && count > 0)
    amplitudes_.assign(amplitudes, amplitudes + count);
  levels_.resize(static_cast<size_t>(count));
  present_.assign(static_cast<size_t>(count), 0);
  if (use_new_initialization) {
    WgLegacyPositionalFactory positional(random.next_long());
    for (int i = 0; i < count; ++i) {
      if (amplitude_at(amplitudes, i) == 0.0)
        continue;
      const int32_t octave = first_octave + i;
      WgLegacyRandom octave_random = positional.from_hash_of("octave_" + std::to_string(octave));
      levels_[static_cast<size_t>(i)].init(octave_random);
      present_[static_cast<size_t>(i)] = 1;
    }
  } else {
    init_legacy_from(random, first_octave, amplitudes, count);
  }
  lowest_freq_input_factor_ = std::pow(2.0, -(-first_octave));
  lowest_freq_value_factor_ = std::pow(2.0, count - 1) / (std::pow(2.0, count) - 1.0);
}

template <typename Random>
void WgPerlinNoise::init_legacy_from(Random& random, int32_t first_octave, const double* amplitudes,
                                     int count) {
  const int32_t zero_octave_index = -first_octave;
  WgImprovedNoise zero_octave;
  zero_octave.init(random);
  if (zero_octave_index >= 0 && zero_octave_index < count) {
    levels_[static_cast<size_t>(zero_octave_index)] = zero_octave;
    if (amplitude_at(amplitudes, zero_octave_index) != 0.0) {
      present_[static_cast<size_t>(zero_octave_index)] = 1;
    }
  }
  for (int i = zero_octave_index - 1; i >= 0; --i) {
    if (i < count) {
      if (amplitude_at(amplitudes, i) != 0.0) {
        levels_[static_cast<size_t>(i)].init(random);
        present_[static_cast<size_t>(i)] = 1;
        continue;
      }
      random.consume_count(262);
      continue;
    }
    random.consume_count(262);
  }
}

double WgPerlinNoise::get_value(double x, double y, double z, double y_scale,
                                double y_fudge) const {
  double value = 0.0;
  double factor = lowest_freq_input_factor_;
  double value_factor = lowest_freq_value_factor_;
  for (int i = 0; i < count_; ++i) {
    if (present_[static_cast<size_t>(i)]) {
      const double noise_value = levels_[static_cast<size_t>(i)].noise(
          wrap(x * factor), wrap(y * factor), wrap(z * factor), y_scale * factor, y_fudge * factor);
      value += amplitudes_[static_cast<size_t>(i)] * noise_value * value_factor;
    }
    factor *= 2.0;
    value_factor /= 2.0;
  }
  return value;
}

const WgImprovedNoise* WgPerlinNoise::octave_noise(int i) const {
  const int index = count_ - 1 - i;
  if (index < 0 || index >= count_)
    return nullptr;
  return present_[static_cast<size_t>(index)] != 0 ? &levels_[static_cast<size_t>(index)] : nullptr;
}

double WgPerlinNoise::wrap(double x) {
  // The fold is the identity for |x| < half a cell: `x / 2^25 + 0.5` is then in
  // [0, 1), so `lfloor` is 0 and the subtraction changes nothing. A generated
  // column is sampled within a few thousand blocks of the origin and the
  // overworld's octave factors are small powers of two, so this is the path
  // essentially every octave takes — and it removes the division and the floor
  // the old form paid three times per octave. Larger coordinates take the same
  // guarded fold as before.
  constexpr double kWrapCell = 3.3554432e7; // 2^25, as in `PerlinNoise.wrap`
  constexpr double kHalfCell = kWrapCell / 2.0;
  if (x > -kHalfCell && x < kHalfCell)
    return x;
  return x - static_cast<double>(wg_lfloor(x / kWrapCell + 0.5)) * kWrapCell;
}

void WgNormalNoise::init(WgRandom& random, int32_t first_octave, const double* amplitudes,
                         int count) {
  init_from(random, first_octave, amplitudes, count, /*use_new_initialization=*/true);
}

void WgNormalNoise::init_legacy_nether(WgLegacyRandom& random, int32_t first_octave,
                                       const double* amplitudes, int count) {
  init_from(random, first_octave, amplitudes, count, /*use_new_initialization=*/false);
}

void WgNormalNoise::init_legacy(WgLegacyRandom& random, int32_t first_octave,
                                const double* amplitudes, int count) {
  init_from(random, first_octave, amplitudes, count, /*use_new_initialization=*/true);
}

template <typename Random>
void WgNormalNoise::init_from(Random& random, int32_t first_octave, const double* amplitudes,
                              int count, bool use_new_initialization) {
  first_ = make_perlin(random, first_octave, amplitudes, count, use_new_initialization);
  second_ = make_perlin(random, first_octave, amplitudes, count, use_new_initialization);
  int32_t min_octave = std::numeric_limits<int32_t>::max();
  int32_t max_octave = std::numeric_limits<int32_t>::min();
  for (int i = 0; i < count; ++i) {
    if (amplitude_at(amplitudes, i) == 0.0)
      continue;
    min_octave = std::min(min_octave, static_cast<int32_t>(i));
    max_octave = std::max(max_octave, static_cast<int32_t>(i));
  }
  // No octave has a non-zero amplitude: every term `get_value` sums is zero,
  // so the answer is zero whatever the factor, and the span above is still the
  // two sentinels — subtracting them would overflow. Leave the factor at zero
  // rather than divide by a span that does not exist.
  if (min_octave > max_octave) {
    value_factor_ = 0.0;
    return;
  }
  // The span is measured in *amplitude-list* indices, not octave numbers.
  value_factor_ = 0.16666666666666666 / expected_deviation(max_octave - min_octave);
}

double WgNormalNoise::get_value(double x, double y, double z) const {
  const double x2 = x * kInputFactor;
  const double y2 = y * kInputFactor;
  const double z2 = z * kInputFactor;
  return (first_.get_value(x, y, z) + second_.get_value(x2, y2, z2)) * value_factor_;
}

void WgBlendedNoise::init(WgRandom& terrain_random, const WgBlendedNoiseParams& params) {
  init_from(terrain_random, params);
}

void WgBlendedNoise::init_legacy(WgLegacyRandom& terrain_random,
                                 const WgBlendedNoiseParams& params) {
  init_from(terrain_random, params);
}

template <typename Random>
void WgBlendedNoise::init_from(Random& terrain_random, const WgBlendedNoiseParams& params) {
  xz_scale_ = params.xz_scale;
  y_scale_ = params.y_scale;
  xz_factor_ = params.xz_factor;
  y_factor_ = params.y_factor;
  smear_scale_multiplier_ = params.smear_scale_multiplier;
  // Three legacy Perlins, in this order, from the one terrain random. The
  // amplitude lists are all ones: sixteen octaves for the two limits and eight
  // for the main noise.
  double limits[16];
  double main[8];
  for (double& amplitude : limits) {
    amplitude = 1.0;
  }
  for (double& amplitude : main) {
    amplitude = 1.0;
  }
  min_limit_ = make_legacy_perlin(terrain_random, -15, limits, 16);
  max_limit_ = make_legacy_perlin(terrain_random, -15, limits, 16);
  main_ = make_legacy_perlin(terrain_random, -7, main, 8);
  xz_multiplier_ = 684.412 * xz_scale_;
  y_multiplier_ = 684.412 * y_scale_;
}

double WgBlendedNoise::compute(int32_t block_x, int32_t block_y, int32_t block_z) const {
  const double limit_x = static_cast<double>(block_x) * xz_multiplier_;
  const double limit_y = static_cast<double>(block_y) * y_multiplier_;
  const double limit_z = static_cast<double>(block_z) * xz_multiplier_;
  const double main_x = limit_x / xz_factor_;
  const double main_y = limit_y / y_factor_;
  const double main_z = limit_z / xz_factor_;
  const double limit_smear = y_multiplier_ * smear_scale_multiplier_;
  const double main_smear = limit_smear / y_factor_;

  double main_noise_value = 0.0;
  double pow = 1.0;
  for (int i = 0; i < 8; ++i) {
    const WgImprovedNoise* noise = main_.octave_noise(i);
    if (noise != nullptr) {
      main_noise_value +=
          noise->noise(WgPerlinNoise::wrap(main_x * pow), WgPerlinNoise::wrap(main_y * pow),
                       WgPerlinNoise::wrap(main_z * pow), main_smear * pow, main_y * pow) /
          pow;
    }
    pow /= 2.0;
  }
  // The blend factor: where the main noise sits between its two extremes.
  const double factor = (main_noise_value / 10.0 + 1.0) / 2.0;
  const bool is_max = factor >= 1.0;
  const bool is_min = factor <= 0.0;

  double blend_min = 0.0;
  double blend_max = 0.0;
  pow = 1.0;
  for (int i = 0; i < 16; ++i) {
    const double wx = WgPerlinNoise::wrap(limit_x * pow);
    const double wy = WgPerlinNoise::wrap(limit_y * pow);
    const double wz = WgPerlinNoise::wrap(limit_z * pow);
    const double y_scale_pow = limit_smear * pow;
    if (!is_max) {
      const WgImprovedNoise* min_noise = min_limit_.octave_noise(i);
      if (min_noise != nullptr) {
        blend_min += min_noise->noise(wx, wy, wz, y_scale_pow, limit_y * pow) / pow;
      }
    }
    if (!is_min) {
      const WgImprovedNoise* max_noise = max_limit_.octave_noise(i);
      if (max_noise != nullptr) {
        blend_max += max_noise->noise(wx, wy, wz, y_scale_pow, limit_y * pow) / pow;
      }
    }
    pow /= 2.0;
  }
  return wg_clamped_lerp(factor, blend_min / 512.0, blend_max / 512.0) / 128.0;
}

// ---- `SimplexNoise` / `PerlinSimplexNoise` ---------------------------------
//
// The legacy simplex stack behind `Biome`'s three static noises. The two
// rounding-sensitive constants are `Math.sqrt(3.0)` computed the same way Java
// computes it (`std::sqrt` is correctly rounded on both), and the `0.5`/
// `(3 - sqrt3) / 6` skew factors derived from it — keeping them as expressions
// rather than decimal literals means the bit patterns cannot drift.
namespace {

const double kSimplexF2 = 0.5 * (std::sqrt(3.0) - 1.0);
const double kSimplexG2 = (3.0 - std::sqrt(3.0)) / 6.0;

inline double simplex_corner(int32_t index, double x, double y, double z, double base) {
  double t = base - x * x - y * y - z * z;
  if (t < 0.0)
    return 0.0;
  t *= t;
  const int32_t* g = kGradient[index];
  return t * t *
         (static_cast<double>(g[0]) * x + static_cast<double>(g[1]) * y +
          static_cast<double>(g[2]) * z);
}

} // namespace

void WgSimplexNoise::init(WgLegacyRandom& random) {
  xo_ = random.next_double() * 256.0;
  yo_ = random.next_double() * 256.0;
  zo_ = random.next_double() * 256.0;
  for (int i = 0; i < 256; ++i) {
    p_[i] = i;
  }
  for (int i = 0; i < 256; ++i) {
    const int32_t offset = random.next_int(256 - i);
    const int32_t tmp = p_[i];
    p_[i] = p_[i + offset];
    p_[i + offset] = tmp;
  }
}

double WgSimplexNoise::get_value(double xin, double yin) const {
  const double s = (xin + yin) * kSimplexF2;
  const int32_t i = wg_floor(xin + s);
  const int32_t j = wg_floor(yin + s);
  const double t = static_cast<double>(i + j) * kSimplexG2;
  const double x0 = xin - (static_cast<double>(i) - t);
  const double y0 = yin - (static_cast<double>(j) - t);
  int32_t i1;
  int32_t j1;
  if (x0 > y0) {
    i1 = 1;
    j1 = 0;
  } else {
    i1 = 0;
    j1 = 1;
  }
  const double x1 = x0 - static_cast<double>(i1) + kSimplexG2;
  const double y1 = y0 - static_cast<double>(j1) + kSimplexG2;
  const double x2 = x0 - 1.0 + 2.0 * kSimplexG2;
  const double y2 = y0 - 1.0 + 2.0 * kSimplexG2;
  const int32_t ii = i & 0xFF;
  const int32_t jj = j & 0xFF;
  const int32_t gi0 = perm(ii + perm(jj)) % 12;
  const int32_t gi1 = perm(ii + i1 + perm(jj + j1)) % 12;
  const int32_t gi2 = perm(ii + 1 + perm(jj + 1)) % 12;
  const double n0 = simplex_corner(gi0, x0, y0, 0.0, 0.5);
  const double n1 = simplex_corner(gi1, x1, y1, 0.0, 0.5);
  const double n2 = simplex_corner(gi2, x2, y2, 0.0, 0.5);
  return 70.0 * (n0 + n1 + n2);
}

void WgPerlinSimplexNoise::init(WgLegacyRandom& random, const int32_t* octaves, int32_t count) {
  if (count <= 0)
    return;
  const int32_t low_freq_octaves = -octaves[0];
  const int32_t high_freq_octaves = octaves[count - 1];
  const int32_t total = low_freq_octaves + high_freq_octaves + 1;
  const int32_t zero_octave_index = high_freq_octaves;
  levels_.resize(static_cast<size_t>(total));
  present_.assign(static_cast<size_t>(total), 0);

  WgSimplexNoise zero_octave;
  zero_octave.init(random);
  bool has_zero = false;
  for (int32_t i = 0; i < count; ++i) {
    if (octaves[i] == 0)
      has_zero = true;
  }
  if (zero_octave_index >= 0 && zero_octave_index < total && has_zero) {
    levels_[static_cast<size_t>(zero_octave_index)] = zero_octave;
    present_[static_cast<size_t>(zero_octave_index)] = 1;
  }
  for (int32_t i = zero_octave_index + 1; i < total; ++i) {
    bool contains = false;
    for (int32_t k = 0; k < count; ++k) {
      if (octaves[k] == zero_octave_index - i)
        contains = true;
    }
    if (i >= 0 && contains) {
      levels_[static_cast<size_t>(i)].init(random);
      present_[static_cast<size_t>(i)] = 1;
    } else {
      random.consume_count(262);
    }
  }
  // `high_freq_octaves > 0` builds the highest octaves from a `WorldgenRandom`
  // seeded off the zero octave's origin; none of the three `Biome` noises use
  // that shape (both sets here top out at octave 0).
  highest_freq_input_factor_ = std::pow(2.0, static_cast<double>(high_freq_octaves));
  highest_freq_value_factor_ = 1.0 / (std::pow(2.0, static_cast<double>(total)) - 1.0);
}

double WgPerlinSimplexNoise::get_value(double x, double y) const {
  double value = 0.0;
  double factor = highest_freq_input_factor_;
  double value_factor = highest_freq_value_factor_;
  for (size_t i = 0; i < levels_.size(); ++i) {
    if (present_[i]) {
      value += levels_[i].get_value(x * factor, y * factor) * value_factor;
    }
    factor /= 2.0;
    value_factor *= 2.0;
  }
  return value;
}

} // namespace mc::world
