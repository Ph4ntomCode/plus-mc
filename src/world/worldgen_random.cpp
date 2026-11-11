// SPDX-License-Identifier: GPL-3.0-or-later
//
// The implementations behind `worldgen_random.h`. Every line here is a
// transcription of `RandomSupport`, `Xoroshiro128PlusPlus` and
// `XoroshiroRandomSource` (26.2) rather than a reconstruction from memory of
// what those classes "probably" do, because the whole point of the layer is
// that a single bit of drift is invisible for one chunk and then, a million
// blocks away, is a different mountain.
//
// Two things are easy to get wrong and are called out where they happen:
// `nextDouble`'s unit constant is the *float* literal, and `Mth.getSeed`'s
// first multiply is a 32-bit one.

#include "world/worldgen_random.h"

#include <openssl/evp.h>

#include <cstddef>

namespace mc::world {
namespace {

constexpr uint64_t rotate_left(uint64_t value, int bits) {
  return (value << bits) | (value >> (64 - bits));
}

} // namespace

uint64_t wg_mix_stafford_13(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; // -4658895280553007687
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; // -7723592293110705685
  return z ^ (z >> 31);
}

WgSeed128 wg_upgrade_seed_to_128bit_unmixed(uint64_t legacy_seed) {
  const uint64_t lo = legacy_seed ^ kWgSilverRatio64;
  return {lo, lo + kWgGoldenRatio64};
}

WgSeed128 wg_upgrade_seed_to_128bit(uint64_t legacy_seed) {
  const WgSeed128 unmixed = wg_upgrade_seed_to_128bit_unmixed(legacy_seed);
  return {wg_mix_stafford_13(unmixed.lo), wg_mix_stafford_13(unmixed.hi)};
}

WgSeed128 wg_seed_from_hash_of(std::string_view name) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1 ||
      EVP_DigestUpdate(ctx, name.data(), name.size()) != 1 ||
      EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len < 16) {
    EVP_MD_CTX_free(ctx);
    return {};
  }
  EVP_MD_CTX_free(ctx);

  // `Longs.fromBytes` reads each half big-endian.
  uint64_t lo = 0;
  uint64_t hi = 0;
  for (int i = 0; i < 8; ++i) {
    lo = (lo << 8) | digest[i];
    hi = (hi << 8) | digest[i + 8];
  }
  return {lo, hi};
}

int64_t wg_mth_get_seed(int32_t x, int32_t y, int32_t z) {
  // `(long)(x * 3129871)` is an *int* multiply that wraps, then sign-extends.
  const auto x_part = static_cast<int32_t>(static_cast<uint32_t>(x) * 3129871u);
  uint64_t seed = static_cast<uint64_t>(static_cast<int64_t>(x_part));
  seed ^= static_cast<uint64_t>(static_cast<int64_t>(z)) * 116129781ull;
  seed ^= static_cast<uint64_t>(static_cast<int64_t>(y));
  seed = seed * seed * 42317861ull + seed * 11ull;
  // Java's `>>` on a long is arithmetic; spell that out rather than rely on
  // an implementation-defined signed shift.
  const uint64_t shifted = seed >> 16;
  const uint64_t sign_fill = (seed & 0x8000000000000000ull) != 0 ? 0xFFFF000000000000ull : 0;
  return static_cast<int64_t>(shifted | sign_fill);
}

WgXoroshiro128PlusPlus::WgXoroshiro128PlusPlus(uint64_t seed_lo, uint64_t seed_hi)
    : seed_lo_(seed_lo), seed_hi_(seed_hi) {
  if ((seed_lo_ | seed_hi_) == 0) {
    seed_lo_ = kWgGoldenRatio64;
    seed_hi_ = kWgSilverRatio64;
  }
}

uint64_t WgXoroshiro128PlusPlus::next_long() {
  const uint64_t s0 = seed_lo_;
  uint64_t s1 = seed_hi_;
  const uint64_t result = rotate_left(s0 + s1, 17) + s0;
  s1 ^= s0;
  seed_lo_ = rotate_left(s0, 49) ^ s1 ^ (s1 << 21);
  seed_hi_ = rotate_left(s1, 28);
  return result;
}

// ---- `LegacyRandomSource` --------------------------------------------------
//
// The 48-bit LCG. Two details the `BitRandomSource` defaults carry: `nextInt`
// is the power-of-two shortcut or the modulo-rejection loop on `next(31)`
// signed *int* arithmetic (Java wraps, so the rejection test wraps too), and
// `nextDouble` combines `next(26)` and `next(27)` and scales by the *float*
// literal, the same constant `XoroshiroRandomSource` uses.
namespace {
constexpr uint64_t kLegacyModulusMask = 0xFFFFFFFFFFFFull;
constexpr uint64_t kLegacyMultiplier = 25214903917ull;
constexpr uint64_t kLegacyIncrement = 11ull;
} // namespace

WgLegacyRandom::WgLegacyRandom(int64_t seed) {
  set_seed(seed);
}

void WgLegacyRandom::set_seed(int64_t seed) {
  seed_ = (static_cast<uint64_t>(seed) ^ 0x5DEECE66Dull) & kLegacyModulusMask;
}

int32_t WgLegacyRandom::next_bits(int bits) {
  seed_ = (seed_ * kLegacyMultiplier + kLegacyIncrement) & kLegacyModulusMask;
  return static_cast<int32_t>(seed_ >> (48 - bits));
}

int32_t WgLegacyRandom::next_int() {
  return next_bits(32);
}

int32_t WgLegacyRandom::next_int(int32_t bound) {
  if (bound <= 0)
    return 0;
  if ((bound & (bound - 1)) == 0)
    return static_cast<int32_t>((static_cast<int64_t>(bound) * next_bits(31)) >> 31);
  while (true) {
    const int32_t sample = next_bits(31);
    const int32_t modulo = sample % bound;
    // `sample - modulo + (bound - 1) < 0` in Java int arithmetic; unsigned
    // here so the wrap is defined.
    const uint32_t check = static_cast<uint32_t>(sample) - static_cast<uint32_t>(modulo) +
                           static_cast<uint32_t>(bound - 1);
    if (static_cast<int32_t>(check) >= 0)
      return modulo;
  }
}

int64_t WgLegacyRandom::next_long() {
  const int64_t upper = next_bits(32);
  const int64_t lower = next_bits(32);
  return (upper << 32) + lower;
}

bool WgLegacyRandom::next_boolean() {
  return next_bits(1) != 0;
}

float WgLegacyRandom::next_float() {
  return static_cast<float>(next_bits(24)) * 5.9604645e-8f;
}

double WgLegacyRandom::next_double() {
  const int64_t combined =
      (static_cast<int64_t>(next_bits(26)) << 27) + static_cast<int64_t>(next_bits(27));
  return static_cast<double>(combined) * static_cast<double>(1.110223e-16f);
}

void WgLegacyRandom::consume_count(int32_t rounds) {
  for (int32_t i = 0; i < rounds; ++i)
    next_int();
}

namespace {
WgXoroshiro128PlusPlus generator_from_legacy_seed(int64_t seed) {
  const WgSeed128 bits = wg_upgrade_seed_to_128bit(static_cast<uint64_t>(seed));
  return WgXoroshiro128PlusPlus(bits.lo, bits.hi);
}
} // namespace

WgRandom::WgRandom(int64_t seed, WgRandomMode mode)
    : rng_(generator_from_legacy_seed(seed)), worldgen_(mode == WgRandomMode::kWorldgen) {}

WgRandom::WgRandom(uint64_t seed_lo, uint64_t seed_hi) : rng_(seed_lo, seed_hi) {}

uint64_t WgRandom::next_long() {
  if (worldgen_) {
    // `BitRandomSource.nextLong` is two `next(32)` reads, not one raw long.
    const int64_t upper = next_bits_int(32);
    const int64_t lower = next_bits_int(32);
    return static_cast<uint64_t>((upper << 32) + lower);
  }
  return rng_.next_long();
}

int32_t WgRandom::next_bits_int(int bits) {
  // `WorldgenRandom.next(bits)` with the source `applyBiomeDecoration` actually
  // constructs — `new WorldgenRandom(new XoroshiroRandomSource(seed))` — is
  // `(int)(randomSource.nextLong() >>> (64 - bits))`, so every decoration draw
  // is a slice of one Xoroshiro long. Feeding this through a 48-bit LCG (the
  // `LegacyRandomSource` shape) desynchronises the stream at the first draw.
  return static_cast<int32_t>(rng_.next_long() >> (64 - bits));
}

int32_t WgRandom::next_int(int32_t bound) {
  if (bound <= 0)
    return 0;
  if (worldgen_) {
    // `BitRandomSource.nextInt(bound)`: `next(31)`, the power-of-two multiply
    // path, and Java's modulo rejection loop otherwise.
    if ((bound & (bound - 1)) == 0)
      return static_cast<int32_t>((static_cast<int64_t>(bound) * next_bits_int(31)) >> 31);
    for (;;) {
      const int32_t sample = next_bits_int(31);
      const int32_t modulo = sample % bound;
      // Java's `sample - modulo + (bound - 1)` wraps in `int`; unsigned here
      // keeps that defined and identical under Clang and GCC.
      const uint32_t check = static_cast<uint32_t>(sample) - static_cast<uint32_t>(modulo) +
                             static_cast<uint32_t>(bound - 1);
      if (static_cast<int32_t>(check) >= 0)
        return modulo;
    }
  }
  // `Integer.toUnsignedLong(nextInt())`: zero-extend the low 32 bits.
  uint64_t random_bits = static_cast<uint32_t>(static_cast<int32_t>(rng_.next_long()));
  uint64_t multiplied = random_bits * static_cast<uint64_t>(bound);
  uint64_t fractional = multiplied & 0xFFFFFFFFull;
  if (fractional < static_cast<uint64_t>(bound)) {
    // `Integer.remainderUnsigned(~bound + 1, bound)` is 2^32 mod bound.
    const uint32_t unbiased_buckets_start =
        static_cast<uint32_t>(-bound) % static_cast<uint32_t>(bound);
    while (fractional < unbiased_buckets_start) {
      random_bits = static_cast<uint32_t>(static_cast<int32_t>(rng_.next_long()));
      multiplied = random_bits * static_cast<uint64_t>(bound);
      fractional = multiplied & 0xFFFFFFFFull;
    }
  }
  return static_cast<int32_t>(multiplied >> 32);
}

bool WgRandom::next_boolean() {
  if (worldgen_)
    return next_bits_int(1) != 0;
  return (rng_.next_long() & 1) != 0;
}

uint64_t WgRandom::next_bits(int bits) {
  return rng_.next_long() >> (64 - bits);
}

float WgRandom::next_float() {
  // `FLOAT_UNIT = 5.9604645E-8f` = 2^-24 to *float* precision, then the product
  // is what the float multiply gives. `BitRandomSource` reads 24 bits through
  // `next`, which is one raw long; the Xoroshiro flavour does the same read.
  if (worldgen_)
    return static_cast<float>(next_bits_int(24)) * 5.9604645e-8f;
  return static_cast<float>(next_bits(24)) * 5.9604645e-8f;
}

double WgRandom::next_double() {
  if (worldgen_) {
    // `BitRandomSource.nextDouble`: two reads, 26 then 27 bits, times 2^-53.
    const int64_t combined = (static_cast<int64_t>(next_bits_int(26)) << 27) + next_bits_int(27);
    return static_cast<double>(combined) * 1.1102230246251565e-16;
  }
  // `DOUBLE_UNIT = (double)1.110223E-16f` — the float literal widened, not
  // 2^-53. The two differ in their last bits, and this constant feeds
  // `ImprovedNoise`'s origin offsets, so it is load-bearing.
  return static_cast<double>(next_bits(53)) * static_cast<double>(1.110223e-16f);
}

void WgRandom::consume_count(int rounds) {
  for (int i = 0; i < rounds; ++i) {
    rng_.next_long();
  }
}

WgPositionalFactory WgRandom::fork_positional() {
  const uint64_t lo = rng_.next_long();
  const uint64_t hi = rng_.next_long();
  return WgPositionalFactory(lo, hi);
}

WgRandom WgPositionalFactory::at(int32_t x, int32_t y, int32_t z) const {
  return WgRandom(static_cast<uint64_t>(wg_mth_get_seed(x, y, z)) ^ seed_lo_, seed_hi_);
}

WgRandom WgPositionalFactory::from_hash_of(std::string_view name) const {
  const WgSeed128 seed = wg_seed_from_hash_of(name);
  return WgRandom(seed.lo ^ seed_lo_, seed.hi ^ seed_hi_);
}

WgRandom WgPositionalFactory::from_seed(int64_t seed) const {
  const uint64_t bits = static_cast<uint64_t>(seed);
  return WgRandom(bits ^ seed_lo_, bits ^ seed_hi_);
}

int32_t wg_java_string_hash(std::string_view text) {
  uint32_t hash = 0;
  for (const char c : text)
    hash = hash * 31u + static_cast<uint8_t>(c);
  return static_cast<int32_t>(hash);
}

WgLegacyRandom WgLegacyPositionalFactory::at(int32_t x, int32_t y, int32_t z) const {
  return WgLegacyRandom(static_cast<int64_t>(wg_mth_get_seed(x, y, z)) ^ seed_);
}

WgLegacyRandom WgLegacyPositionalFactory::from_hash_of(std::string_view name) const {
  return WgLegacyRandom(static_cast<int64_t>(wg_java_string_hash(name)) ^ seed_);
}

} // namespace mc::world
