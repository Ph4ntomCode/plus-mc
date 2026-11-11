// SPDX-License-Identifier: GPL-3.0-or-later

#include "world/worldgen_state.h"

#include <string_view>
#include <utility>

#include "world/worldgen_end_table.h"
#include "world/worldgen_nether_table.h"

namespace mc::world {

WgRandomState::WgRandomState(int64_t seed, bool build_legacy_noises) {
  // `RandomState(this.random = settings.getRandomSource().newInstance(seed).forkPositional())`.
  root_ = WgRandom(seed).fork_positional();
  aquifer_random_ = root_.from_hash_of("minecraft:aquifer").fork_positional();
  ore_random_ = root_.from_hash_of("minecraft:ore").fork_positional();

  WgLegacyRandom end_island_random(seed);
  end_island_random.consume_count(17292);
  end_island_noise_.init(end_island_random);

  // `Noises.instantiate` builds every one of these the same way — a positional
  // stream named after the parameter set — so the two `legacy_random` sets are
  // built from their data-pack parameters too, which is also what the reference
  // dump does. (`RandomState` would override those two with instances built on
  // a `LegacyRandomSource`; see the header for why this port does not.)
  noises_.resize(sizeof(kWgNoiseParams) / sizeof(kWgNoiseParams[0]));
  for (size_t i = 0; i < noises_.size(); ++i) {
    const WgNoiseParams& params = kWgNoiseParams[i];
    WgRandom random = root_.from_hash_of(params.name);
    noises_[i].init(random, params.first_octave, kWgNoiseAmplitudes + params.amplitude_offset,
                    static_cast<int>(params.amplitude_count));
  }
  // `RandomState.noises` holds the data-pack instances above. The router is a
  // separate wiring pass: only the two legacy Nether climate noises are
  // replaced there, from `LegacyRandomSource(seed + offset)`.
  wired_noises_ = noises_;
  for (size_t i = 0; i < wired_noises_.size(); ++i) {
    const WgNoiseParams& params = kWgNoiseParams[i];
    if (!params.legacy_random)
      continue;
    const int64_t offset = std::string_view(params.name) == "minecraft:nether/vegetation" ? 1 : 0;
    WgLegacyRandom legacy(seed + offset);
    wired_noises_[i].init_legacy_nether(legacy, params.first_octave,
                                        kWgNoiseAmplitudes + params.amplitude_offset,
                                        static_cast<int>(params.amplitude_count));
  }

  // The overworld's `BlendedNoise` is name-derived (`minecraft:terrain`), not
  // order-derived. Its table indexes this vector through
  // `WgDensityTable::blended_base`.
  const auto add_blended = [&](const WgBlendedNoiseParams* params, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      WgRandom terrain_random = root_.from_hash_of("minecraft:terrain");
      WgBlendedNoise blended;
      blended.init(terrain_random, params[i]);
      blended_.push_back(std::move(blended));
    }
  };
  add_blended(kWgBlendedNoise, std::size(kWgBlendedNoise));
  // Nether and End set `legacy_random_source`, so vanilla builds their
  // `BlendedNoise` from a fresh `LegacyRandomSource(seed)` rather than the
  // positional "minecraft:terrain" stream.
  const auto add_legacy_blended = [&](const WgBlendedNoiseParams* params, size_t count) {
    for (size_t i = 0; i < count; ++i) {
      WgLegacyRandom legacy(seed);
      WgBlendedNoise blended;
      blended.init_legacy(legacy, params[i]);
      blended_.push_back(std::move(blended));
    }
  };
  add_legacy_blended(kWgNetherBlendedNoise, std::size(kWgNetherBlendedNoise));
  add_legacy_blended(kWgEndBlendedNoise, std::size(kWgEndBlendedNoise));

  // A legacy-rooted `RandomState`'s data-pack noises come from the same
  // `LegacyPositionalRandomFactory` chain. Keep them separate from
  // `noises_`: the router fixtures pin the registry instances above, while the
  // Nether surface reads this set.
  if (!build_legacy_noises)
    return;
  WgLegacyRandom legacy_root(seed);
  WgLegacyPositionalFactory legacy_factory(legacy_root.next_long());
  legacy_noises_.resize(noises_.size());
  for (size_t i = 0; i < legacy_noises_.size(); ++i) {
    const WgNoiseParams& params = kWgNoiseParams[i];
    WgLegacyRandom random = legacy_factory.from_hash_of(params.name);
    legacy_noises_[i].init_legacy(random, params.first_octave,
                                  kWgNoiseAmplitudes + params.amplitude_offset,
                                  static_cast<int>(params.amplitude_count));
  }
}

} // namespace mc::world
