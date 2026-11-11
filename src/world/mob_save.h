// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mob population's save file (Phase 4): entity ids that survive a restart.
//
// Vanilla stores living entities per chunk in the world's `entities/` region
// files, each row a full entity NBT (UUID, Pos, Motion, the mob's own data) and
// the entity *id* is only the run's counter, re-issued every boot. This engine
// has no entity region writer (and no per-chunk entity store at all: the
// population is one vector on the tick thread), so the two things the rest of
// the engine treats as stable are what this file carries instead:
//
//   * the entity **id** of every live mob. `mate_entity_id`, `parent_entity_id`
//     and the death/kill records all name mobs by id, so an id that changes
//     across a restart breaks every reference that was held when the process
//     died. Ids are never reused inside a run, and this file keeps them unique
//     across one too: `NextEntityId` restores the run's counter above every id
//     the file holds.
//   * the **animal state** those references live in: `AgeableMob.age` (negative
//     is a baby), `Animal.inLove`, `BreedGoal.loveTime`, the breed partner and
//     the `FollowParentGoal` parent. A baby's parent pointer and a breeding
//     pair's partner pointer are exactly the cross-mob references a restart
//     would otherwise drop, so they are the reason this file exists.
//
// The file is `<world>/mobs.dat`: gzip NBT, one root compound (the same
// container level.dat uses, and the same `DataVersion` stamp). It is this
// engine's own file — a vanilla server would not read it, and the server does
// not read vanilla's `entities/` region files — which docs/PHASE4.md records.
//
// What is deliberately *not* in the file (transient state, rebuilt on load):
// velocity, the AI's target/path/goal bookkeeping (a target names a *player*,
// whose entity id is session-scoped), burning, active effects, and the live
// fields of a villager's offers (uses, demand, special price): a villager's
// offer *list* is rebuilt from its profession/level, and the deterministic
// per-entity RNG means the rebuilt rows are the same trades.
//
// The codec is pure (`nbt::Value` in and out) so the schema is unit-tested
// without the filesystem; the two file wrappers add the gzip and the atomic
// temp+rename that level.dat's writer uses.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "world/mob.h"
#include "world/nbt.h"

namespace mc::world {

// The file's name under the world root, and the DataVersion it is stamped with
// (the version `AnvilWorld::ensure_level_dat` writes level.dat with).
constexpr const char* kMobPopulationFileName = "mobs.dat";
constexpr int kMobPopulationDataVersion = 5023;

// The population a file read produced.
struct MobPopulation {
  std::vector<Mob> mobs;
  // The id the next spawned entity takes: the saved counter, or one above every
  // id the file held when the file's own value had fallen behind.
  int64_t next_entity_id = 1;
};

// The population as the file's root compound:
//
//   root {
//     DataVersion    int   5023
//     NextEntityId   long  Entity.getId's counter at save time
//     Mobs           list<compound>, one per live mob {
//       Id                   int              entity id (the point of the file)
//       Type                 string           generated table name
//       Pos                  list<double>[3]  feet centre
//       Yaw                  float
//       Health               float
//       Age                  int              AgeableMob.age (negative = baby)
//       ForcedAge            int
//       InLove               int              Animal.inLove
//       LoveTime             int              BreedGoal.loveTime
//       Mate                 int              BreedGoal.partner id (-1 none)
//       Parent               int              FollowParentGoal.parent id (-1 none)
//       NoActionTime         int
//       PersistenceRequired  byte
//       Villager             compound, present on a minecraft:villager {
//         Type         byte  VillagerType id
//         Profession   byte  VillagerProfession id
//         Level        int
//         Xp           int
//         OffersBuilt  byte  AbstractVillager.offers != null
//       }
//     }
//   }
//
// Only live, typed, unremoved mobs are written. A mob whose `type` is null is
// skipped rather than guessed at.
nbt::Value mob_population_to_nbt(const std::vector<Mob>& mobs, int64_t next_entity_id);

// Reads that compound back. A row that is missing its id, its type or its
// position, or whose type name is not in the generated table, is skipped (its
// id dies with it, exactly as a vanilla entity with no type would); only a root
// that is not this file's shape at all is rejected. `out` is untouched on a
// rejection.
bool mob_population_from_nbt(const nbt::Value& root, MobPopulation* out);

// Writes `<world_dir>/mobs.dat` (gzip, temp+rename: a crash never leaves a torn
// file). An absent directory is an error. Returns false on any I/O error.
bool write_mob_population(const std::filesystem::path& world_dir, const std::vector<Mob>& mobs,
                          int64_t next_entity_id);

// Reads `<world_dir>/mobs.dat`. A *missing* file is success with an empty
// population (a fresh world); a present file that cannot be read, inflated or
// parsed is a failure. On failure `out` is untouched.
bool read_mob_population(const std::filesystem::path& world_dir, MobPopulation* out);

} // namespace mc::world
