// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mob tick's answer contract, pinned as a digest.
//
// Every change to `src/world/mob.cpp` so far has been argued with a private
// probe in a `/tmp` directory: the same five scenes ticked against the old and
// the new translation unit, with a digest of everything one tick produced. That
// probe is this file. It is checked in so the argument is a `ctest` target
// instead of a directory that gets cleaned:
//
//   ctest -R mob-digest
//
// What it pins: for every tick of every scene, the `MobTickReport` (moved,
// grew_up, attacks with their damage, hurt, flags_changed, deaths with their
// loot data, despawned, births with the child's position and parents, and the
// villager vectors), then every field of every surviving mob in population
// order -- position and velocity, yaw, health, the timers, the goal and target
// state, the path and its waypoints, the stuck-detection fields, the effect
// set, the burning state, the damage attribution and the villager state -- and
// finally the number and values of the uniform draws the tick consumed. A
// change that moves any of those changes the digest, and the gate fails.
//
// The scenes are the shapes the tick actually branches on, one per goal family:
//
//   los      zombies and cows around players, so acquisition and the melee
//            re-plan ask for line of sight (and clip on the wall grid).
//   despawn  a despawn-distance spread with the three states mixed in: far and
//            discardable, far but persistent, and near-but-idle past 600 ticks
//            (the 1-in-800 branch).
//   flee     zombies whose target leaves FOLLOW_RANGE while the player walks
//            away: the retention check's range branch, where the sight answer
//            is discarded -- the dead walk this gate exists for.
//   still    cows held off their stroll: the per-mob fixed cost.
//   mixed    all of it in one population, the shape a server ticks.
//   pen      forty in-love pairs and forty calves: the only scene past
//            `MobGrid`'s population switch (80), so `BreedGoal.getFreePartner` and
//            `FollowParentGoal.canUse` are answered from the buckets instead of
//            a population scan. Its digest matches the scan arm's, which is
//            what makes the grid's tie-breaking and its one-block widen a
//            checked claim rather than a comment: `ctest -R mob-grid-ab` links
//            a second binary from this same file with `world/mob.cpp`
//            compiled for the scan (`PLUSMC_MOB_GRID_OFF`), runs both, and
//            compares every scene and the aggregate.
//
// The world is a floor plus a one-block-high wall grid every seven blocks. Mob
// spawn positions land in the grid on purpose: several start overlapping a wall
// cell, which is the collision-escape case `Entity.move` has and the
// pre-escape port froze. Those mobs moving (or not) is part of the contract.
//
// To re-pin after a *deliberate* behaviour change, run the binary with `--print`
// and put the printed aggregate digest in `kExpectedDigest`, saying why in the
// commit. A re-pin is the acknowledgement that mob answers moved.
//
// Determinism note: the draws come from this file's own xorshift, not
// `std::mt19937_64`, and doubles are mixed at a fixed 1e-6 quantum instead of by
// their raw bits, so the constant is not a prisoner of one standard library's
// distribution or one libm's last ulp. Everything the tick decides -- draws,
// branches, path cells, ids, timers, booleans -- is exact, and the decisions
// were measured stable across -O0/-O2/-O3 and both `-ffp-contract` settings (all
// five read the same digest). If a toolchain ever disagrees, fix the corpus
// rather than pinning a second number.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "world/mob.h"

using namespace mc::world;

namespace {

// The pinned aggregate, computed from the corpus below (see the header). It is
// the post-`mob_box_free_escaping` value: several corpus mobs spawn inside the
// wall grid, so a build that goes back to a target-box-only collision reads a
// different number (and is a regression). It gained the `pen` scene when
// `MobGrid` landed: the aggregate was read once with the grid on and once with
// `kMinPopulation` raised so the same corpus ran the old population scan, and
// the two read the same number -- the scene's whole point.
constexpr uint64_t kExpectedDigest = 0xA749A2A200926058ull;

struct Digest {
  uint64_t h = 1469598103934665603ull;
  void mix(uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<uint8_t>(v >> (i * 8));
      h *= 1099511628211ull;
    }
  }
  void mix_i(int64_t v) {
    mix(static_cast<uint64_t>(v));
  }
  // A double at a fixed quantum: see the determinism note in the header.
  void mix_d(double v) {
    mix_i(static_cast<int64_t>(std::llround(v * 1.0e6)));
  }
  void mix_f(float v) {
    mix_d(static_cast<double>(v));
  }
  void mix_s(const std::string& s) {
    mix(s.size());
    for (char c : s)
      mix(static_cast<uint8_t>(c));
  }
  void mix_cell(const NavCell& c) {
    mix_i(c.x);
    mix_i(c.y);
    mix_i(c.z);
  }
};

// This file's own generator: xorshift64, seeded per scene. A fixed sequence on
// every standard library (see the header).
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double unit() {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }
};

// The world the scenes tick against: a floor plus a one-block-high grid every
// seven blocks, so walkers corner (and LOS finds walls to clip on), and so a
// mob placed on the lattice can start overlapping a wall cell.
MobEnvironment make_env(int players, std::function<double()> unit) {
  MobEnvironment env;
  env.is_solid = [](int x, int y, int z) {
    if (y <= 0)
      return true;
    return y == 1 && (std::abs(x % 7) == 0 || std::abs(z % 7) == 0);
  };
  for (int i = 0; i < players; ++i) {
    MobPlayer p;
    p.entity_id = 9000 + i;
    p.x = 3.5 + (i % 5) * 11.0;
    p.z = 3.5 + (i / 5) * 11.0;
    p.y = 1.0;
    env.players.push_back(p);
  }
  env.unit_random = std::move(unit);
  return env;
}

Mob make(const char* name, int32_t id, double x, double z) {
  Mob mob;
  mob.entity_id = id;
  const GeneratedMob* type = mob_by_name(name);
  if (type == nullptr) {
    std::fprintf(stderr, "mob-digest: unknown mob %s\n", name);
    std::exit(2);
  }
  mob_place(mob, *type, MobEnvironment{}, x, 1.0, z, 0.0f);
  return mob;
}

void mix_mob(Digest& d, const Mob& m) {
  d.mix_i(m.entity_id);
  d.mix_s(m.type ? m.type->name : "");
  d.mix_d(m.x);
  d.mix_d(m.y);
  d.mix_d(m.z);
  d.mix_d(m.vx);
  d.mix_d(m.vy);
  d.mix_d(m.vz);
  d.mix_f(m.yaw);
  d.mix_f(m.health);
  d.mix_f(m.invulnerable_time);
  d.mix_f(m.last_hurt);
  d.mix(m.on_ground ? 1 : 0);
  d.mix(m.removed ? 1 : 0);
  d.mix(m.horizontal_collision ? 1 : 0);
  d.mix_i(m.age);
  d.mix(m.glowing ? 1 : 0);
  d.mix_i(m.breed_age);
  d.mix_i(m.forced_age);
  d.mix_i(m.in_love);
  d.mix_i(m.love_time);
  d.mix_i(m.mate_entity_id);
  d.mix_s(m.love_cause);
  d.mix_i(m.parent_entity_id);
  d.mix_i(m.follow_repath);
  d.mix_i(m.no_action_time);
  d.mix(m.persistence_required ? 1 : 0);
  d.mix_d(m.fall_distance);
  d.mix_d(m.fall_start_y);
  d.mix(m.falling ? 1 : 0);
  d.mix_i(static_cast<int>(m.goal));
  d.mix_i(m.target_entity_id);
  d.mix_i(m.attack_cooldown);
  d.mix_i(m.target_scan_cooldown);
  d.mix_i(m.path_cooldown);
  d.mix_i(m.stray_cooldown);
  d.mix_d(m.move_speed_scale);
  d.mix(m.target_visible ? 1 : 0);
  d.mix_i(m.unseen_ticks);
  d.mix(m.target_provoked ? 1 : 0);
  d.mix_i(m.provoked_entity_id);
  d.mix(m.provoked_fresh ? 1 : 0);
  d.mix_i(static_cast<int64_t>(m.path.points.size()));
  for (const NavCell& c : m.path.points)
    d.mix_cell(c);
  // How far along the path the follower is. Without it a mob that advanced a
  // waypoint while keeping the same points would hash identically.
  d.mix_i(static_cast<int64_t>(m.path.next));
  d.mix_d(m.path_to_x);
  d.mix_d(m.path_to_y);
  d.mix_d(m.path_to_z);
  d.mix(m.has_path_goal ? 1 : 0);
  d.mix_i(m.nav_tick);
  d.mix_i(m.last_stuck_check);
  d.mix_d(m.last_stuck_check_x);
  d.mix_d(m.last_stuck_check_y);
  d.mix_d(m.last_stuck_check_z);
  d.mix(m.is_stuck ? 1 : 0);
  d.mix(m.has_timeout_node ? 1 : 0);
  d.mix_cell(m.timeout_node);
  d.mix_i(m.timeout_timer);
  d.mix_i(m.timeout_last_check);
  d.mix_d(m.timeout_limit);
  d.mix_i(m.last_attacker_entity_id);
  d.mix_s(m.last_attacker_name);
  d.mix(m.killed_by_player ? 1 : 0);
  // Fire + effects state the tick can move.
  d.mix_i(m.burning.remaining_ticks);
  d.mix_i(m.burning.immune_ticks);
  d.mix_i(static_cast<int64_t>(m.effects.size()));
  for (const MobEffectInstance& e : m.effects.all()) {
    d.mix_i(e.effect);
    d.mix_i(e.duration);
    d.mix_i(e.amplifier);
  }
  // The villager half: every field the tick can move, plus the offer rows a
  // career increase appends (`offers` is the one part of the state the tick
  // itself can grow). The item components behind a trade result are
  // session-owned and cannot change during a tick.
  d.mix_i(m.villager.type);
  d.mix_i(static_cast<int>(m.villager.profession));
  d.mix_i(m.villager.level);
  d.mix_i(m.villager.xp);
  d.mix(m.villager.offers_built ? 1 : 0);
  d.mix_i(m.villager.update_merchant_timer);
  d.mix(m.villager.increase_level_on_update ? 1 : 0);
  d.mix_i(m.villager.unhappy_counter);
  d.mix_i(m.villager.trading_player);
  d.mix_i(m.villager.last_traded_player);
  d.mix_i(static_cast<int64_t>(m.villager.offers.size()));
  for (const MerchantOffer& o : m.villager.offers) {
    d.mix_i(o.cost_a.item);
    d.mix_i(o.cost_a.count);
    d.mix(o.has_cost_b ? 1 : 0);
    d.mix_i(o.cost_b.item);
    d.mix_i(o.cost_b.count);
    d.mix_i(o.result.item);
    d.mix_i(o.result.count);
    d.mix_i(o.result.damage);
    d.mix_i(o.uses);
    d.mix_i(o.max_uses);
    d.mix(o.reward_exp ? 1 : 0);
    d.mix_i(o.special_price_diff);
    d.mix_i(o.demand);
    d.mix_f(o.price_multiplier);
    d.mix_i(o.xp);
  }
}

void mix_report(Digest& d, const MobTickReport& r) {
  for (const int32_t id : r.moved)
    d.mix_i(id);
  d.mix(0x4d4f56ull);
  for (const int32_t id : r.grew_up)
    d.mix_i(id);
  d.mix(0x47524full);
  for (const MobAttack& a : r.attacks) {
    d.mix_i(a.mob_entity_id);
    d.mix_i(a.player_entity_id);
    d.mix_f(a.damage);
    d.mix_s(a.mob_name);
  }
  d.mix(0x415454ull);
  for (const int32_t id : r.hurt)
    d.mix_i(id);
  d.mix(0x48555254ull);
  for (const int32_t id : r.flags_changed)
    d.mix_i(id);
  d.mix(0x464c4147ull);
  for (const MobDeath& dd : r.deaths) {
    d.mix_i(dd.entity_id);
    d.mix_s(dd.type ? dd.type->name : "");
    d.mix_d(dd.x);
    d.mix_d(dd.y);
    d.mix_d(dd.z);
    d.mix(dd.killed_by_player ? 1 : 0);
    d.mix_i(dd.killer_entity_id);
    d.mix_s(dd.killer_name);
  }
  d.mix(0x44454144ull);
  for (const int32_t id : r.despawned)
    d.mix_i(id);
  d.mix(0x44455350ull);
  for (const MobBirth& b : r.births) {
    d.mix_i(b.parent_entity_id);
    d.mix_i(b.partner_entity_id);
    d.mix_s(b.type ? b.type->name : "");
    d.mix_d(b.x);
    d.mix_d(b.y);
    d.mix_d(b.z);
    d.mix_f(b.yaw);
    d.mix_s(b.love_cause);
  }
  d.mix(0x42495254ull);
  for (const int32_t id : r.villager_traded)
    d.mix_i(id);
  d.mix(0x54524144ull);
  for (const int32_t id : r.villager_leveled)
    d.mix_i(id);
  d.mix(0x4c564cull);
}

struct Scene {
  const char* name;
  const char* shape;
  int players;
  int ticks;
  void (*fill)(std::vector<Mob>&);
  // Player 0 walks this many blocks per tick along +x (a chase that breaks off
  // when the target leaves FOLLOW_RANGE), so the retention scan's range test is
  // exercised rather than always answered "in range".
  double player_dx = 0.0;
};

void fill_los(std::vector<Mob>& mobs) {
  // Zombies near the players (they acquire, chase and hold by sight) and cows
  // that stroll: LOS is asked for both acquisition and the melee re-plan gate.
  for (int i = 0; i < 48; ++i) {
    const double x = 2.5 + (i % 12) * 4.0;
    const double z = 2.5 + (i / 12) * 6.0;
    mobs.push_back(make(i % 4 == 0 ? "cow" : "zombie", 100 + i, x, z));
  }
}

void fill_despawn(std::vector<Mob>& mobs) {
  // A spread of distances from the player cluster (the players sit on a 5x2
  // grid at 11-block pitch, so "nearest" varies from 4 to ~150 blocks) with the
  // three despawn states mixed in: far and discardable, far but persistent,
  // and near-but-idle (the 1-in-800 branch once no_action_time runs out).
  for (int i = 0; i < 60; ++i) {
    const char* name = (i % 3 == 0) ? "cow" : (i % 3 == 1 ? "zombie" : "sheep");
    const double x = -40.0 + (i % 10) * 14.0;
    const double z = -20.0 + (i / 10) * 16.0;
    Mob m = make(name, 300 + i, x, z);
    if (i % 5 == 1)
      m.persistence_required = true;
    if (i % 4 == 2)
      m.no_action_time = 601 + i;
    mobs.push_back(std::move(m));
  }
}

void fill_flee(std::vector<Mob>& mobs) {
  // Zombies with targets in range that walk out of it: the retention check's
  // range branch, where the sight test's answer is discarded anyway.
  for (int i = 0; i < 32; ++i)
    mobs.push_back(make("zombie", 900 + i, 2.5 + (i % 8) * 2.0, 2.5 + (i / 8) * 2.0));
  for (int i = 0; i < 8; ++i)
    mobs.push_back(make("cow", 950 + i, 1.5 + i * 2.0, 30.5));
}

void fill_still(std::vector<Mob>& mobs) {
  // Nobody in reach of a stroll: the per-mob fixed cost and the despawn scan.
  for (int i = 0; i < 40; ++i) {
    Mob m = make("cow", 500 + i, 1.5 + (i % 8) * 2.0, 1.5 + (i / 8) * 2.0);
    m.stray_cooldown = 1000000;
    mobs.push_back(std::move(m));
  }
}

void fill_mixed(std::vector<Mob>& mobs) {
  for (int i = 0; i < 64; ++i) {
    const double x = 2.5 + (i % 8) * 3.0;
    const double z = 2.5 + (i / 8) * 3.0;
    switch (i % 4) {
    case 0:
      mobs.push_back(make("zombie", 700 + i, x, z));
      break;
    case 1: {
      Mob baby = make("cow", 700 + i, x, z);
      baby.breed_age = kAnimalBabyStartAge;
      mobs.push_back(std::move(baby));
      break;
    }
    case 2: {
      Mob cow = make("cow", 700 + i, x, z);
      cow.stray_cooldown = 1000000;
      mobs.push_back(std::move(cow));
      break;
    }
    default:
      mobs.push_back(make("sheep", 700 + i, x, z));
      break;
    }
  }
}

void fill_pen(std::vector<Mob>& mobs) {
  // A breeding pen: forty cells on a 12-block pitch, each with two cows in love
  // two blocks apart (they breed once, about sixty ticks in) and a calf that
  // follows the pair. 120 animals, so `MobGrid::kMinPopulation` is passed and
  // both animal scans read the buckets; the pitch keeps a neighbouring cell's
  // cows more than eight blocks from the breeding pair, so the partner a cow
  // resolves is its own cell's.
  for (int i = 0; i < 40; ++i) {
    const double cx = 2.5 + (i % 8) * 12.0;
    const double cz = 2.5 + (i / 8) * 12.0;
    Mob a = make("cow", 1300 + 2 * i, cx, cz);
    a.in_love = kAnimalInLoveTicks;
    mobs.push_back(std::move(a));
    Mob b = make("cow", 1301 + 2 * i, cx + 2.0, cz);
    b.in_love = kAnimalInLoveTicks;
    mobs.push_back(std::move(b));
    Mob calf = make("cow", 1500 + i, cx + 5.0, cz + 2.0);
    calf.breed_age = kAnimalBabyStartAge;
    calf.parent_entity_id = 1300 + 2 * i;
    mobs.push_back(std::move(calf));
  }
}

bool print_mode = false;
long total_ticks = 0;
uint64_t total_draws = 0;

} // namespace

int main(int argc, char** argv) {
  print_mode = argc > 1 && std::strcmp(argv[1], "--print") == 0;
  const Scene scenes[] = {
      {"los", "48 mobs, 10 players", 10, 400, fill_los},
      {"despawn", "60 mobs, 10 players", 10, 400, fill_despawn},
      {"flee", "40 mobs, a player walking", 1, 400, fill_flee, /*player_dx=*/0.25},
      {"still", "40 idle cows", 0, 300, fill_still},
      {"mixed", "64 mobs, 10 players", 10, 300, fill_mixed},
      {"pen", "120 animals, 6 players", 6, 300, fill_pen},
  };

  Digest total;
  Digest draw_digest;
  for (const Scene& scene : scenes) {
    Rng rng(0x5EEDFA11ull);
    uint64_t draws = 0;
    Digest scene_digest;
    MobEnvironment env = make_env(scene.players, [&rng, &draw_digest, &draws] {
      ++draws;
      const double v = rng.unit();
      draw_digest.mix_d(v);
      return v;
    });
    std::vector<Mob> mobs;
    scene.fill(mobs);
    for (int t = 0; t < scene.ticks; ++t) {
      if (scene.player_dx != 0.0 && !env.players.empty()) {
        MobPlayer& p = env.players[0];
        p.x += scene.player_dx;
        if (p.x > 90.0)
          p.x = -90.0;
      }
      const MobTickReport report = tick_mobs(mobs, env);
      ++total_ticks;
      scene_digest.mix_i(t);
      scene_digest.mix(static_cast<uint64_t>(mobs.size()));
      mix_report(scene_digest, report);
      for (const Mob& m : mobs)
        mix_mob(scene_digest, m);
    }
    scene_digest.mix(draws);
    total_draws += draws;
    total.mix(scene_digest.h);
    if (print_mode) {
      std::printf("  %-8s %-28s digest %016llx  (%d ticks, %llu draws)\n", scene.name, scene.shape,
                  static_cast<unsigned long long>(scene_digest.h), scene.ticks,
                  static_cast<unsigned long long>(draws));
    }
  }
  total.mix(draw_digest.h);

  // Which arm this run took, said out loud: `ctest -R mob-grid-ab` compares
  // this binary against its `PLUSMC_MOB_GRID_OFF` twin, so two binaries that
  // both used the grid would agree for the wrong reason. `pen` is the only
  // scene past the switch, hence its population here.
  std::printf("mob-digest: the pen scene's 120 animals will answer from the %s\n",
              mob_grid_enabled_for(120) ? "grid" : "population scan");

  // A corpus sanity check before the digest is trusted: a generator bug that
  // emptied the scenes must not read as a passing gate.
  if (total_ticks < 1000 || total_draws < 500) {
    std::fprintf(stderr,
                 "mob-digest: the corpus is degenerate (ticks %ld, draws %llu) -- fix the "
                 "scenes before reading the digest\n",
                 total_ticks, static_cast<unsigned long long>(total_draws));
    return 1;
  }

  const unsigned long long got = static_cast<unsigned long long>(total.h);
  std::printf("mob-digest: %ld ticks, %llu draws, %zu scenes\n", total_ticks,
              static_cast<unsigned long long>(total_draws), sizeof(scenes) / sizeof(scenes[0]));
  std::printf("mob-digest: aggregate %016llx\n", got);

  if (print_mode) {
    std::printf("mob-digest: --print, not comparing (re-pin kExpectedDigest to this value only "
                "for a deliberate behaviour change)\n");
    return 0;
  }
  if (total.h != kExpectedDigest) {
    std::fprintf(stderr,
                 "\nmob-digest: FAIL -- the answers changed.\n"
                 "  expected %016llx\n"
                 "  got      %016llx\n"
                 "Some tick_mobs answer, RNG draw, report vector or mob field is not what it was.\n"
                 "If that is intended, re-pin kExpectedDigest (run with --print) and say why in "
                 "the commit; if it is not, the change is a mob-behaviour regression.\n",
                 static_cast<unsigned long long>(kExpectedDigest), got);
    return 1;
  }
  std::printf("mob-digest: ok (answers match the pinned contract)\n");
  return 0;
}
