// SPDX-License-Identifier: GPL-3.0-or-later
//
// What one mob tick costs, split by the work it is doing. The server's own
// benches (`bench/botnet.py`, `bench/soak.py`) price the whole tick under
// players; when a profile says the mob loop is the expensive part, this probe
// says *which* part of it. It is not part of the build and nothing asserts on
// its output; build it against the core library and read the table:
//
//   clang++ -O2 -std=c++17 -Isrc bench/mob_tick_probe.cpp \
//       build-relight/src/libplusmc-core.a -o /tmp/mob_tick_probe
//   /tmp/mob_tick_probe            # 200 mobs, 600 ticks per scene
//   /tmp/mob_tick_probe 500 1500   # more mobs, longer runs
//
// The scenes, and what each isolates:
//
//   idle    cows held off their stroll (`stray_cooldown` long): the per-mob
//           fixed cost, no path, no player — despawn check, age, effects,
//           target scan against an empty player list, and the goal fallthrough.
//   stroll  cows left alone on a walkable floor with a wall grid: pathfinding
//           (re-plans every 4-10 ticks), the path follower, collision and the
//           physics.
//   chase   zombies with a player standing 6 blocks off, on the same floor:
//           the target scan, the melee path re-plans and the attack branch.
//   follow  baby cows, one adult each: FollowParentGoal's candidate scan (an
//           O(mobs) box query per baby per goal tick) and its re-plans.
//   mixed   all four kinds in one population: the shape the server actually
//           ticks.
//
// Report the median of the printed passes, not a single one: the figures move
// with whatever else the box is doing. Compare the scenes to attribute, and
// compare a before/after pair at the same scene and mob count to price a
// change.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "world/mob.h"

using namespace mc::world;

namespace {

MobEnvironment floor_world(bool walls) {
  MobEnvironment env;
  env.is_solid = [walls](int x, int y, int z) {
    if (y <= 0)
      return true;
    if (!walls)
      return false;
    // A one-block-high grid every seven blocks, so walking mobs have to corner
    // and re-plan rather than stroll in a straight line.
    return y == 1 && (std::abs(x % 7) == 0 || std::abs(z % 7) == 0);
  };
  env.unit_random = [] { return 0.5; };
  return env;
}

Mob make(const char* name, int id, double x, double z, bool baby = false) {
  Mob mob;
  mob.entity_id = id;
  const GeneratedMob* type = mob_by_name(name);
  if (type == nullptr) {
    std::fprintf(stderr, "unknown mob %s\n", name);
    std::exit(2);
  }
  MobEnvironment env = floor_world(false);
  mob_place(mob, *type, env, x, 1.0, z, 0.0f);
  if (baby)
    mob.breed_age = kAnimalBabyStartAge;
  return mob;
}

double run_scene(const std::string& scene, int mobs, int players, int ticks) {
  MobEnvironment env = floor_world(true);
  for (int i = 0; i < players; ++i) {
    MobPlayer p;
    p.entity_id = 10000 + i;
    const int gx = i % 20, gz = i / 20;
    p.x = 0.5 + gx * 7.0;
    p.y = 1.0;
    p.z = 0.5 + gz * 7.0;
    env.players.push_back(p);
  }
  std::vector<Mob> population;
  population.reserve(static_cast<size_t>(mobs) * 2);
  for (int i = 0; i < mobs; ++i) {
    const double x = 0.5 + (i % 40) * 3.0;
    const double z = 0.5 + (i / 40) * 3.0;
    if (scene == "idle") {
      Mob cow = make("cow", i + 1, x, z);
      cow.stray_cooldown = 1000000;
      population.push_back(std::move(cow));
    } else if (scene == "stroll") {
      population.push_back(make("cow", i + 1, x, z));
    } else if (scene == "chase") {
      population.push_back(make("zombie", i + 1, x, z));
    } else if (scene == "follow") {
      Mob baby = make("cow", i + 1, x, z, /*baby=*/true);
      Mob parent = make("cow", 100000 + i, x + 4.0, z);
      parent.stray_cooldown = 1000000;
      population.push_back(std::move(baby));
      population.push_back(std::move(parent));
    } else { // mixed
      const int kind = i % 4;
      if (kind == 0) {
        population.push_back(make("zombie", i + 1, x, z));
      } else if (kind == 1) {
        Mob baby = make("cow", i + 1, x, z, /*baby=*/true);
        population.push_back(std::move(baby));
      } else if (kind == 2) {
        Mob cow = make("cow", i + 1, x, z);
        cow.stray_cooldown = 1000000;
        population.push_back(std::move(cow));
      } else {
        population.push_back(make("sheep", i + 1, x, z));
      }
    }
  }
  // Warm-up, then the timed run.
  for (int i = 0; i < 20; ++i)
    tick_mobs(population, env);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < ticks; ++i)
    tick_mobs(population, env);
  const auto stop = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(stop - start).count();
  std::printf("%-7s mobs=%-5zu players=%-3d ticks=%-5d %8.2f ms  %6.1f ns/mob/tick\n",
              scene.c_str(), population.size(), players, ticks, ms,
              ms * 1.0e6 / (static_cast<double>(population.size()) * ticks));
  return ms;
}

} // namespace

int main(int argc, char** argv) {
  const int mobs = argc > 1 ? std::atoi(argv[1]) : 200;
  const int ticks = argc > 2 ? std::atoi(argv[2]) : 600;
  const int players = argc > 3 ? std::atoi(argv[3]) : 20;
  std::printf("mob tick probe: %d mobs, %d ticks, %d players\n", mobs, ticks, players);
  for (const char* scene : {"idle", "stroll", "chase", "follow", "mixed"})
    run_scene(scene, mobs, players, ticks);
  return 0;
}
