// SPDX-License-Identifier: GPL-3.0-or-later
//
// The noise sources the overworld router reads: `ImprovedNoise`, `PerlinNoise`,
// `NormalNoise` and `BlendedNoise`, 1:1 with `net.minecraft.world.level.
// levelgen.synth`.
//
// The one structural difference from vanilla is ownership: vanilla allocates
// every octave's `ImprovedNoise` on the heap and leaves holes for the octaves
// whose amplitude is zero. Here a `WgPerlinNoise` owns a `std::vector` of
// octaves, sized to the amplitude list, with a parallel "is this octave
// present" flag — the same shape, without the pointer chasing.
//
// Everything is constructed from a `WgRandom` (the `XoroshiroRandomSource`
// port) in the exact order vanilla draws from it, because the draw order is
// part of what a seed means:
//
//   * new-style (`PerlinNoise.create`): one `forkPositional()`, then each
//     non-zero octave taken from `fromHashOf("octave_<n>")` — name-derived, so
//     the order octaves are created in does not matter.
//   * legacy (`createLegacyForBlendedNoise`): sequential draws, and the octave
//     at index `-firstOctave` is built *first*, with the others after it in
//     descending index order, consuming 262 longs per skipped zero-amplitude
//     octave.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "world/worldgen_random.h"
#include "world/worldgen_table.h"

namespace mc::world {

// `SimplexNoise`: the older simplex implementation `PerlinSimplexNoise` builds
// on, and the one the surface system's legacy biome noises sample. Only the
// 2D `getValue(x, y)` is ported: every `Biome` noise call in the surface path
// is 2D, and the 3D form (plus the octave seeding that uses it) belongs to a
// `highFreqOctaves > 0` construction none of these noises use.
//
// `p` is 512 entries long and the shuffle fills the first 256, but every read
// goes through `SimplexNoise.p(int)`, which masks with `& 0xFF`. Vanilla's
// indices (`ii + p(jj)` and friends) genuinely do reach past 255 and rely on
// that mask, so `perm` below is the accessor, not a cosmetic detail.
class WgSimplexNoise {
public:
  WgSimplexNoise() = default;
  void init(WgLegacyRandom& random);

  double get_value(double x, double y) const;

private:
  int32_t perm(int32_t x) const {
    return p_[x & 0xFF];
  }

  int32_t p_[512] = {};
  double xo_ = 0.0;
  double yo_ = 0.0;
  double zo_ = 0.0;
};

// `PerlinSimplexNoise`: a sum of `SimplexNoise` octaves, weighted by
// `highestFreqValueFactor` and fed `x * 2^octave` by `highestFreqInputFactor`.
// The octave set must be sorted ascending and its highest octave at most 0 —
// the shapes `Biome`'s three static noises use ([0] and [-2, -1, 0]).
class WgPerlinSimplexNoise {
public:
  WgPerlinSimplexNoise() = default;
  void init(WgLegacyRandom& random, const int32_t* octaves, int32_t count);

  // `getValue(x, y, false)`. The `useNoiseStart` form is never used by the
  // surface path and is deliberately absent.
  double get_value(double x, double y) const;

private:
  std::vector<WgSimplexNoise> levels_;
  std::vector<uint8_t> present_;
  double highest_freq_value_factor_ = 1.0;
  double highest_freq_input_factor_ = 1.0;
};

// `ImprovedNoise`: 256 permutation entries, Fisher-Yates shuffled, plus the
// three origin offsets that all three come from `nextDouble`.
class WgImprovedNoise {
public:
  WgImprovedNoise() = default;
  void init(WgRandom& random);
  void init(WgLegacyRandom& random);

  double noise(double x, double y, double z) const {
    return noise(x, y, z, 0.0, 0.0);
  }
  // The `yScale`/`yFudge` overload `BlendedNoise` uses; `yr` is the *unfudged*
  // fractional part, which is why it is passed along separately.
  double noise(double x, double y, double z, double y_scale, double y_fudge) const;

private:
  template <typename Random> void init_from(Random& random);

  int perm(int index) const {
    return p_[index & 0xFF];
  }
  static double grad_dot(int hash, double x, double y, double z);
  double sample_and_lerp(int x, int y, int z, double xr, double yr, double zr,
                         double yr_original) const;

  double xo_ = 0.0;
  double yo_ = 0.0;
  double zo_ = 0.0;
  uint8_t p_[256] = {};
};

// `PerlinNoise.createLegacyForBlendedNoise`'s skip: 262 longs.
void wg_skip_octave(WgRandom& random);

// `PerlinNoise`, for one amplitude list. `first_octave` is the octave index of
// `amplitudes[0]`.
class WgPerlinNoise {
public:
  WgPerlinNoise() = default;
  // `use_new_initialization` is `PerlinNoise`'s own flag: true for
  // `NormalNoise.create`, false for the legacy blended-noise construction.
  WgPerlinNoise(WgRandom& random, int32_t first_octave, const double* amplitudes, int count,
                bool use_new_initialization);
  // `createLegacyForBlendedNoise` / `createLegacyForLegacyNetherBiome`: the
  // same legacy octave order, but drawn from a `LegacyRandomSource`.
  WgPerlinNoise(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes, int count);
  // The normal `PerlinNoise.create` path over a legacy source: positional fork,
  // then one `fromHashOf("octave_N")` per non-zero octave.
  WgPerlinNoise(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes, int count,
                bool use_new_initialization);

  double get_value(double x, double y, double z) const {
    return get_value(x, y, z, 0.0, 0.0);
  }
  double get_value(double x, double y, double z, double y_scale, double y_fudge) const;

  // `getOctaveNoise(i)` reads from the *end* of the level array: the octave at
  // index 0 is the lowest frequency one, which is the last index.
  const WgImprovedNoise* octave_noise(int i) const;

  static double wrap(double x);

private:
  template <typename Random>
  void init_legacy_from(Random& random, int32_t first_octave, const double* amplitudes, int count);

  std::vector<WgImprovedNoise> levels_;
  std::vector<uint8_t> present_;
  // Owned, not borrowed. Callers build the amplitude list in a local array
  // (`place_geode`'s geode noise, `WgBlendedNoise::init`'s two limit arrays)
  // and the noise is sampled after that scope ends; a raw pointer read the
  // stack slot's next occupant, which differed by compiler.
  std::vector<double> amplitudes_;
  int count_ = 0;
  double lowest_freq_input_factor_ = 0.0;
  double lowest_freq_value_factor_ = 0.0;
};

// `NormalNoise`: two `PerlinNoise`s of the same parameters, the second sampled
// at `INPUT_FACTOR` times the input, summed and scaled.
class WgNormalNoise {
public:
  static constexpr double kInputFactor = 1.0181268882175227;

  WgNormalNoise() = default;
  void init(WgRandom& random, int32_t first_octave, const double* amplitudes, int count);
  // `NormalNoise.createLegacyNetherBiome`: two legacy Perlins from one
  // `LegacyRandomSource`, with no positional fork.
  void init_legacy_nether(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes,
                          int count);
  // The same legacy source under `NormalNoise.create`'s normal initialization
  // (the mode every data-pack noise uses, including on a legacy-rooted state).
  void init_legacy(WgLegacyRandom& random, int32_t first_octave, const double* amplitudes,
                   int count);

  double get_value(double x, double y, double z) const;

private:
  template <typename Random>
  void init_from(Random& random, int32_t first_octave, const double* amplitudes, int count,
                 bool use_new_initialization);

  WgPerlinNoise first_;
  WgPerlinNoise second_;
  double value_factor_ = 1.0;
};

// `BlendedNoise`: the eight-octave "main" noise that picks the blend factor
// between a sixteen-octave min-limit and max-limit noise.
class WgBlendedNoise {
public:
  WgBlendedNoise() = default;
  void init(WgRandom& terrain_random, const WgBlendedNoiseParams& params);
  void init_legacy(WgLegacyRandom& terrain_random, const WgBlendedNoiseParams& params);

  double compute(int32_t block_x, int32_t block_y, int32_t block_z) const;

private:
  template <typename Random>
  void init_from(Random& terrain_random, const WgBlendedNoiseParams& params);

  WgPerlinNoise min_limit_;
  WgPerlinNoise max_limit_;
  WgPerlinNoise main_;
  double xz_scale_ = 0.0;
  double y_scale_ = 0.0;
  double xz_factor_ = 0.0;
  double y_factor_ = 0.0;
  double smear_scale_multiplier_ = 0.0;
  double xz_multiplier_ = 0.0;
  double y_multiplier_ = 0.0;
};

} // namespace mc::world
