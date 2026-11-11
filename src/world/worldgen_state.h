// SPDX-License-Identifier: GPL-3.0-or-later
//
// `RandomState`'s seed wiring, minus the registries: one seed turns into the
// root positional factory, that factory turns into the aquifer and ore
// factories and into every `NormalNoise` the data pack defines, and the
// dimension's `NoiseGeneratorSettings` decides how `BlendedNoise` is seeded.
//
// The order of `RandomState`'s own forks is load-bearing and is reproduced
// here exactly:
//
//   root   = newXoroshiro(seed).forkPositional()          // consumes 2 longs
//   aquifer = root.fromHashOf("minecraft:aquifer").forkPositional()
//   ore     = root.fromHashOf("minecraft:ore").forkPositional()
//
// Everything after that is *name*-derived and therefore order-independent:
// `Noises.instantiate` builds the data-pack instances from
// `root.fromHashOf(name)`. The router is a separate wiring pass: the overworld's
// `BlendedNoise` comes from the positional "minecraft:terrain" stream, while a
// settings preset with `legacy_random_source` gets a fresh
// `LegacyRandomSource(seed)` for its blended noise. The same pass replaces the
// two legacy Nether climate noises (`seed` and `seed + 1`), which is why the
// registry set (`noises_`) and the router's holder set (`wired_noises_`) are
// kept separately.
#pragma once

#include <cstdint>
#include <vector>

#include "world/worldgen_noise.h"
#include "world/worldgen_random.h"
#include "world/worldgen_table.h"

namespace mc::world {

class WgRandomState {
public:
  explicit WgRandomState(int64_t seed, bool build_legacy_noises = false);

  // `legacy` reads the surface-system set built from `LegacyRandomSource`;
  // router/density sampling keeps using the data-pack instances above.
  const WgNormalNoise& noise(int32_t index, bool legacy = false) const {
    return legacy && !legacy_noises_.empty() ? legacy_noises_[index] : noises_[index];
  }
  // The router's holder for one registry noise. Vanilla's `RandomState.noises`
  // stays as the data pack built it; `NoiseWiringHelper` only replaces the two
  // legacy Nether climate holders it hands to the router.
  const WgNormalNoise& wired_noise(int32_t index) const {
    return wired_noises_[index];
  }
  const WgBlendedNoise& blended(int32_t index) const {
    return blended_[index];
  }
  // `DensityFunctions.EndIslandDensityFunction(seed)`: the one code-built
  // density function whose simplex table is seeded directly from the world
  // seed (17292 discarded draws first), not through a positional factory.
  const WgSimplexNoise& end_island_noise() const {
    return end_island_noise_;
  }

  const WgPositionalFactory& root_random() const {
    return root_;
  }
  const WgPositionalFactory& aquifer_random() const {
    return aquifer_random_;
  }
  const WgPositionalFactory& ore_random() const {
    return ore_random_;
  }

  // `RandomState.getOrCreateRandomFactory(name)`, used by the surface system
  // and by anything else that wants its own named stream.
  WgPositionalFactory named_random(std::string_view name) const {
    return root_.from_hash_of(name).fork_positional();
  }

private:
  WgPositionalFactory root_;
  WgPositionalFactory aquifer_random_;
  WgPositionalFactory ore_random_;
  std::vector<WgNormalNoise> noises_;
  std::vector<WgNormalNoise> legacy_noises_;
  std::vector<WgNormalNoise> wired_noises_;
  std::vector<WgBlendedNoise> blended_;
  WgSimplexNoise end_island_noise_;
};

} // namespace mc::world
