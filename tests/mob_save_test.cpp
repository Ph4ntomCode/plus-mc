// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test: the mob population's save codec (world/mob_save.{h,cpp}).
//
// What it pins:
//   * a full population round-trips through `nbt::serialize` -> `nbt::parse`,
//     with every persisted field intact — most importantly the entity ids and
//     the two cross-mob references (`Mate`, `Parent`) that goals and love
//     clocks hold, plus the animal clocks (`Age`, `InLove`, `LoveTime`);
//   * `NextEntityId` comes back, and a file whose counter has fallen behind its
//     own rows is raised above them (an id must never be re-issued while a
//     restored reference names it);
//   * a villager's trade state survives and its offer list is rebuilt to the
//     same rows the live level-ups built (the deterministic per-entity RNG);
//   * a row that cannot be reconstructed (unknown type name, no id, no Pos, not
//     a compound) is skipped rather than failing the population, and only a
//     root that is not this file's shape at all is rejected;
//   * a dead or type-less mob is not written, and missing optional fields fall
//     back to their defaults (a full-health non-animal).
#include <cstdio>
#include <string>
#include <vector>

#include "world/mob.h"
#include "world/mob_save.h"
#include "world/nbt.h"
#include "world/trade.h"

using mc::world::GeneratedMob;
using mc::world::Mob;
using mc::world::MobPopulation;
using mc::world::nbt::Value;
namespace nbt = mc::world::nbt;

namespace {

bool ok = true;

void expect(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ok = false;
  }
}

Mob make_mob(const GeneratedMob& type, int32_t id, double x, double y, double z) {
  Mob mob;
  mob.type = &type;
  mob.entity_id = id;
  mob.x = x;
  mob.y = y;
  mob.z = z;
  mob.health = type.max_health;
  return mob;
}

} // namespace

int main() {
  const GeneratedMob* cow = mc::world::mob_by_name("minecraft:cow");
  const GeneratedMob* zombie = mc::world::mob_by_name("minecraft:zombie");
  const GeneratedMob* villager = mc::world::mob_by_name("minecraft:villager");
  expect(cow != nullptr && zombie != nullptr && villager != nullptr, "table rows exist");
  if (!ok)
    return 1;

  // ---- the round trip -------------------------------------------------------
  std::vector<Mob> mobs;
  // A breeding pair: the two cows hold each other's id and their love clocks.
  Mob cow_a = make_mob(*cow, 7, 1.5, 64.0, 2.5);
  cow_a.yaw = 90.0f;
  cow_a.in_love = 587;
  cow_a.love_time = 12;
  cow_a.mate_entity_id = 9;
  // `Animal.addAdditionalSaveData`: the love cause is part of the animal's own
  // saved state (`EntityReference.store(loveCause, "LoveCause")`), so a restart
  // inside the love window keeps whoever fed the pair.
  cow_a.love_cause = "Alice";
  mobs.push_back(cow_a);
  Mob cow_b = make_mob(*cow, 9, 3.5, 64.0, 2.5);
  cow_b.in_love = 587;
  cow_b.love_time = 12;
  cow_b.mate_entity_id = 7;
  // ...and the pair's first child, following the initiator (id 7): negative age
  // (a baby), a forced-age bank, and the parent pointer a FollowParentGoal holds.
  Mob calf = make_mob(*cow, 11, 1.5, 64.0, 2.5);
  calf.breed_age = mc::world::kAnimalBabyStartAge;
  calf.forced_age = 120;
  calf.parent_entity_id = 7;
  mobs.push_back(calf);
  mobs.push_back(cow_b);
  // A hostile, one of whose kind must not grow an `age=`/`love=` tail: it is
  // not an `Animal`, so only its own fields are written.
  Mob zed = make_mob(*zombie, 10, -4.0, 63.0, 8.25);
  zed.health = 13.5f;
  zed.yaw = -45.0f;
  zed.no_action_time = 33;
  zed.persistence_required = true;
  mobs.push_back(zed);
  // A villager that had opened its trade screen once and risen to level 3: the
  // state a restart must keep, with the offer list rebuilt from the file.
  Mob trader = make_mob(*villager, 12, 10.0, 64.0, 10.0);
  trader.villager.type = mc::world::kVillagerTaiga;
  trader.villager.profession = mc::world::kVillagerFarmer;
  trader.villager.level = 3;
  trader.villager.xp = 95;
  trader.villager.offers_built = true;
  mc::world::add_villager_level_offers(&trader.villager, trader.entity_id, 1);
  mc::world::add_villager_level_offers(&trader.villager, trader.entity_id, 2);
  mc::world::add_villager_level_offers(&trader.villager, trader.entity_id, 3);
  mobs.push_back(trader);
  // A corpse and a type-less mob are not written: neither is in the level.
  Mob dead = make_mob(*zombie, 13, 0.0, 0.0, 0.0);
  dead.health = 0.0f;
  mobs.push_back(dead);
  Mob untyped;
  untyped.entity_id = 14;
  mobs.push_back(untyped);

  const Value root = mc::world::mob_population_to_nbt(mobs, 500);
  const std::vector<uint8_t> bytes = nbt::serialize(root);
  const auto parsed = nbt::parse(bytes.data(), bytes.size());
  expect(parsed.has_value(), "the file parses");
  MobPopulation pop;
  expect(mc::world::mob_population_from_nbt(*parsed, &pop), "the root is this file's shape");
  expect(pop.mobs.size() == 5, "two parents, a calf, a zombie and a villager");
  expect(pop.next_entity_id == 500, "NextEntityId is the saved counter");
  if (pop.mobs.size() == 5) {
    const Mob& a = pop.mobs[0];
    expect(a.entity_id == 7 && a.type == cow, "cow a: id and type");
    expect(a.x == 1.5 && a.y == 64.0 && a.z == 2.5, "cow a: position");
    expect(a.yaw == 90.0f, "cow a: yaw");
    expect(a.in_love == 587 && a.love_time == 12, "cow a: the love clock");
    expect(a.love_cause == "Alice", "cow a: the love cause survives the file");
    expect(a.mate_entity_id == 9, "cow a: the breed partner is the other cow's id");
    expect(a.breed_age == 0, "cow a: an adult");
    const Mob& b = pop.mobs[1];
    expect(b.entity_id == 11 && b.type == cow, "calf: id and type");
    expect(b.breed_age == mc::world::kAnimalBabyStartAge && b.baby(),
           "calf: negative age, still a baby");
    expect(b.forced_age == 120, "calf: the forced-age bank");
    expect(b.parent_entity_id == 7, "calf: the follow parent is its initiator's id");
    expect(b.health == cow->max_health, "calf: a missing Health falls back to max");
    expect(pop.mobs[2].entity_id == 9 && pop.mobs[2].mate_entity_id == 7,
           "cow b: the partner link points back");
    const Mob& z = pop.mobs[3];
    expect(z.entity_id == 10 && z.type == zombie, "zombie: id and type");
    expect(z.health == 13.5f && z.yaw == -45.0f, "zombie: health and yaw");
    expect(z.no_action_time == 33 && z.persistence_required, "zombie: despawn state");
    expect(!z.animal(), "a zombie is not an Animal");
  }
  // The villager is the last row: the file keeps the vector's own order.
  if (pop.mobs.size() == 5) {
    const Mob& v = pop.mobs[4];
    expect(v.entity_id == 12 && v.is_villager(), "villager: id and type");
    expect(v.villager.type == mc::world::kVillagerTaiga, "villager: type");
    expect(v.villager.profession == mc::world::kVillagerFarmer, "villager: profession");
    expect(v.villager.level == 3 && v.villager.xp == 95, "villager: level and xp");
    expect(v.villager.offers_built, "villager: offers were built");
    expect(v.villager.offers.size() == trader.villager.offers.size(),
           "villager: the offer list is rebuilt to the same size");
    if (v.villager.offers.size() == trader.villager.offers.size() && !v.villager.offers.empty()) {
      // The deterministic per-entity RNG makes the rows themselves the same
      // trades — the first one's item and cost included.
      expect(v.villager.offers[0].cost_a.item == trader.villager.offers[0].cost_a.item &&
                 v.villager.offers[0].cost_a.count == trader.villager.offers[0].cost_a.count,
             "villager: the rebuilt rows are the same trades");
      expect(v.villager.offers.back().cost_a.item == trader.villager.offers.back().cost_a.item,
             "villager: including the top level's");
    }
  }

  // ---- the counter never falls behind the ids the file holds ----------------
  {
    std::vector<Mob> few{make_mob(*zombie, 42, 0.0, 0.0, 0.0)};
    const Value r = mc::world::mob_population_to_nbt(few, 2);
    MobPopulation p;
    expect(mc::world::mob_population_from_nbt(r, &p), "a stale counter still reads");
    expect(p.next_entity_id == 43, "the counter is raised above the file's own ids");
  }

  // ---- rows that cannot be reconstructed are skipped ------------------------
  {
    // A hand-made population: one good row, then an unknown type, a row with no
    // Pos, a row with no Id, and a non-compound item.
    const Value good = nbt::make_compound(
        {{"Id", nbt::make_int(5)},
         {"Type", nbt::make_string("minecraft:pig")},
         {"Pos", nbt::make_list(nbt::Tag::Double, {nbt::make_double(0.0), nbt::make_double(64.0),
                                                   nbt::make_double(0.0)})}});
    const Value unknown = nbt::make_compound(
        {{"Id", nbt::make_int(6)},
         {"Type", nbt::make_string("minecraft:not_a_mob")},
         {"Pos", nbt::make_list(nbt::Tag::Double, {nbt::make_double(0.0), nbt::make_double(0.0),
                                                   nbt::make_double(0.0)})}});
    const Value no_pos =
        nbt::make_compound({{"Id", nbt::make_int(7)}, {"Type", nbt::make_string("minecraft:pig")}});
    const Value no_id = nbt::make_compound(
        {{"Type", nbt::make_string("minecraft:pig")},
         {"Pos", nbt::make_list(nbt::Tag::Double, {nbt::make_double(0.0), nbt::make_double(0.0),
                                                   nbt::make_double(0.0)})}});
    const Value bad_pos = nbt::make_compound({{"Id", nbt::make_int(8)},
                                              {"Type", nbt::make_string("minecraft:pig")},
                                              {"Pos", nbt::make_string("nope")}});
    const Value root_bad = nbt::make_compound(
        {{"Mobs", nbt::make_list(nbt::Tag::Compound,
                                 {good, unknown, no_pos, no_id, bad_pos, nbt::make_int(3)})}});
    MobPopulation p;
    expect(mc::world::mob_population_from_nbt(root_bad, &p), "one bad row is not fatal");
    expect(p.mobs.size() == 1 && p.mobs[0].entity_id == 5, "only the good row survives");
    expect(p.mobs[0].health == mc::world::mob_by_name("minecraft:pig")->max_health,
           "an omitted Health is the type's max");
    expect(p.mobs[0].mate_entity_id == -1 && p.mobs[0].parent_entity_id == -1 &&
               p.mobs[0].in_love == 0,
           "an omitted reference is -1 and an omitted clock is 0");
  }
  // ...but a root that is not this file at all is rejected, and `out` is
  // untouched.
  {
    MobPopulation p;
    p.next_entity_id = 99;
    expect(!mc::world::mob_population_from_nbt(nbt::make_int(1), &p), "a non-compound root fails");
    expect(!mc::world::mob_population_from_nbt(
               nbt::make_compound({{"Mobs", nbt::make_string("not a list")}}), &p),
           "a Mobs field that is not a list fails");
    expect(p.next_entity_id == 99, "a rejected file leaves the output alone");
  }
  // ---- an empty or absent population ----------------------------------------
  {
    const Value r = mc::world::mob_population_to_nbt({}, 1);
    MobPopulation p;
    p.next_entity_id = 5;
    expect(mc::world::mob_population_from_nbt(r, &p), "an empty population reads");
    expect(p.mobs.empty() && p.next_entity_id == 1, "empty in, empty out (with its counter)");
  }

  if (!ok) {
    std::printf("FAIL: mob-save\n");
    return 1;
  }
  std::printf("PASS: mob-save\n");
  return 0;
}
