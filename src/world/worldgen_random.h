// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seed stack vanilla's world generation is built on (Phase 4, worldgen K1).
//
// Every terrain decision traces back to one `long` seed, and the chain from
// that seed to a `NormalNoise` sample is exact arithmetic with no gameplay
// choices in it — which is why it is ported 1:1 rather than approximated:
//
//   * `RandomSupport.upgradeSeedTo128bit` — `seed ^ 0x6A09E667F3BCC909`,
//     `+ 0x9E3779B97F4A7C15` (both as unsigned wraparound), then Stafford's
//     mix-13 on each half. That 128-bit pair is the `Xoroshiro128PlusPlus`
//     state.
//   * `Xoroshiro128PlusPlus` — the `+ +` scrambler: `rotl(s0 + s1, 17) + s0`
//     as the output, `rotl(s0, 49) ^ (s1 ^ s0) ^ ((s1 ^ s0) << 21)` and
//     `rotl(s1 ^ s0, 28)` as the next state. A zero state is replaced by the
//     golden/silver constants, exactly as the constructor does.
//   * `XoroshiroRandomSource` — the bounded-int algorithm (multiply-high with
//     the unbiased-bucket rejection loop) and, importantly, the two unit
//     constants: `nextFloat` is `nextBits(24) * 2^-24f` and `nextDouble` is
//     `nextBits(53) * (double)1.110223e-16f`, i.e. the *float* literal's value
//     converted to double — not 2^-53 itself. The fixture pins both bit
//     patterns; copying 2^-53 here would have been silently wrong.
//   * `PositionalRandomFactory` — `at(x, y, z)` seeds from `Mth.getSeed`, and
//     `fromHashOf(name)` from the MD5 of the name's UTF-8 bytes, both XORed
//     into the factory's own 128-bit state. That is how two noise functions
//     with different names get different streams from one seed, and why a
//     noise sample does not depend on the order the noises were created in.
#pragma once

#include <cstdint>
#include <string_view>

namespace mc::world {

class WgRandom;

// RandomSupport.GOLDEN_RATIO_64 / SILVER_RATIO_64, as unsigned.
constexpr uint64_t kWgGoldenRatio64 = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kWgSilverRatio64 = 0x6A09E667F3BCC909ull;

struct WgSeed128 {
  uint64_t lo = 0;
  uint64_t hi = 0;

  WgSeed128 xor_with(uint64_t other_lo, uint64_t other_hi) const {
    return {lo ^ other_lo, hi ^ other_hi};
  }
};

// `RandomSupport.mixStafford13`.
uint64_t wg_mix_stafford_13(uint64_t z);

// `RandomSupport.upgradeSeedTo128bit` (and its unmixed first half).
WgSeed128 wg_upgrade_seed_to_128bit_unmixed(uint64_t legacy_seed);
WgSeed128 wg_upgrade_seed_to_128bit(uint64_t legacy_seed);

// `RandomSupport.seedFromHashOf`: MD5 (OpenSSL) of the name, first eight bytes
// as one big-endian long, next eight as the other.
WgSeed128 wg_seed_from_hash_of(std::string_view name);

// `Mth.getSeed(x, y, z)`: the positional seed an `at()` lookup starts from.
int64_t wg_mth_get_seed(int32_t x, int32_t y, int32_t z);

// `LegacyRandomSource` (the 48-bit LCG behind `java.util.Random`), as the
// `BitRandomSource` default methods read it. The surface system's two legacy
// noises (`Biome.TEMPERATURE_NOISE` and friends) are constructed from it, so
// the exact draw order of `worldgen_noise.h`'s simplex classes is pinned by
// the same LCG the game used. Only the `BitRandomSource` methods the simplex
// constructors call are here — no Gaussian, no fork.
class WgLegacyRandom {
public:
  explicit WgLegacyRandom(int64_t seed);

  // `LegacyRandomSource.setSeed`. The carver driver reseeds one instance per
  // (source chunk, carver) pair (`WorldgenRandom.setLargeFeatureSeed`), so the
  // seed is part of the state rather than the constructor's only input.
  void set_seed(int64_t seed);

  int32_t next_int();              // next(32)
  int32_t next_int(int32_t bound); // BitRandomSource.nextInt(bound)
  int64_t next_long();
  bool next_boolean();
  float next_float();
  double next_double();
  void consume_count(int32_t rounds);

private:
  int32_t next_bits(int bits);
  uint64_t seed_ = 0;
};

class WgXoroshiro128PlusPlus {
public:
  WgXoroshiro128PlusPlus(uint64_t seed_lo, uint64_t seed_hi);

  uint64_t next_long();

private:
  uint64_t seed_lo_;
  uint64_t seed_hi_;
};

// `XoroshiroRandomSource`'s positional flavour
// (`XoroshiroPositionalRandomFactory`).
class WgPositionalFactory {
public:
  WgPositionalFactory() = default;
  WgPositionalFactory(uint64_t seed_lo, uint64_t seed_hi) : seed_lo_(seed_lo), seed_hi_(seed_hi) {}

  WgRandom at(int32_t x, int32_t y, int32_t z) const;
  WgRandom from_hash_of(std::string_view name) const;
  WgRandom from_seed(int64_t seed) const;

  uint64_t seed_lo() const {
    return seed_lo_;
  }
  uint64_t seed_hi() const {
    return seed_hi_;
  }

private:
  uint64_t seed_lo_ = 0;
  uint64_t seed_hi_ = 0;
};

// `LegacyPositionalRandomFactory`: the positional flavour of
// `LegacyRandomSource`, used by `RandomState` for settings with
// `legacy_random_source: true` (the Nether and End). Its `fromHashOf` is
// `String.hashCode`-based, not the MD5 the Xoroshiro factory uses, and its
// `at` seeds the 48-bit LCG.
class WgLegacyPositionalFactory {
public:
  WgLegacyPositionalFactory() = default;
  explicit WgLegacyPositionalFactory(int64_t seed) : seed_(seed) {}

  WgLegacyRandom at(int32_t x, int32_t y, int32_t z) const;
  WgLegacyRandom from_hash_of(std::string_view name) const;

  int64_t seed() const {
    return seed_;
  }

private:
  int64_t seed_ = 0;
};

// `String.hashCode` over UTF-16 code units, which for the ASCII identifiers the
// data pack uses is one byte per character.
int32_t wg_java_string_hash(std::string_view text);

// The two RNG facades worldgen talks through. `XoroshiroRandomSource` answers
// `nextInt(bound)` with the multiply-high algorithm and reads `nextFloat` /
// `nextDouble` from a single 53-bit draw. `WorldgenRandom` (a
// `BitRandomSource`) answers them through `next(bits)` on the raw stream, which
// is a different sequence. Noise factories use the first; decoration's
// `setFeatureSeed` stream is the second: `applyBiomeDecoration` builds
// `new WorldgenRandom(new XoroshiroRandomSource(seed))`, so each draw is a
// slice of the same Xoroshiro long stream.
enum class WgRandomMode { kXoroshiro, kWorldgen };

// `XoroshiroRandomSource`. Only the pieces worldgen uses are here; the Gaussian
// sampler and the `Codec` are not part of terrain.
class WgRandom {
public:
  explicit WgRandom(int64_t seed, WgRandomMode mode = WgRandomMode::kXoroshiro);
  WgRandom(uint64_t seed_lo, uint64_t seed_hi);

  uint64_t next_long();
  // `nextInt(bound)` — bound > 0.
  int32_t next_int(int32_t bound);
  bool next_boolean();
  float next_float();
  double next_double();
  void consume_count(int rounds);

  // `forkPositional()`: consumes two longs and hands them to the factory. It is
  // not const because of that consumption, which is what makes the order of
  // forks part of the seed's meaning.
  WgPositionalFactory fork_positional();

  WgXoroshiro128PlusPlus& generator() {
    return rng_;
  }

private:
  uint64_t next_bits(int bits);
  // `WorldgenRandom.next(bits)` = `randomSource.nextLong() >>> (64 - bits)`,
  // returned as the signed `int` Java's cast produces.
  int32_t next_bits_int(int bits);

  WgXoroshiro128PlusPlus rng_;
  bool worldgen_ = false;
};

} // namespace mc::world
