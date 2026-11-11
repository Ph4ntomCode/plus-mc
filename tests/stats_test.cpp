// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the statistics model (Phase 4 milestone M):
//   * the nine StatType ids in `Stats.java`'s field order, and the 77 custom
//     stats in `BuiltInRegistries.CUSTOM_STAT` order (leave_game 0, play_time 1,
//     mob_kills 33, interact_with_smithing_table 76) with their names round
//     tripping through `custom_stat_id`;
//   * the `Block` registry ids the state table derives (air 0, stone 1, dirt 9,
//     cobblestone 12 — vanilla's own numbers) and the inverse;
//   * `Stat.buildName`'s criterion names for all four value registries, and
//     `ObjectiveCriteria.byName`'s parse of them back;
//   * `StatsCounter`: default zero, set, increment's saturation at
//     `Integer.MAX_VALUE`, and the map's contents.
#include <climits>
#include <cstdio>
#include <optional>
#include <string>

#include "world/item.h"
#include "world/mob_table.h"
#include "world/stats.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    const std::string va = (a), vb = (b);                                                          \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a,      \
                   va.c_str(), vb.c_str());                                                        \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

int main() {
  // ---- the two registry orders ----
  CHECK_EQ(kStatMined, 0);
  CHECK_EQ(kStatCrafted, 1);
  CHECK_EQ(kStatUsed, 2);
  CHECK_EQ(kStatBroken, 3);
  CHECK_EQ(kStatPickedUp, 4);
  CHECK_EQ(kStatDropped, 5);
  CHECK_EQ(kStatKilled, 6);
  CHECK_EQ(kStatKilledBy, 7);
  CHECK_EQ(kStatCustom, 8);
  CHECK_EQ(kStatTypeCount, 9);
  CHECK_EQ(kCustomStatCount, 77);
  CHECK_EQ(kCustomLeaveGame, 0);
  CHECK_EQ(kCustomPlayTime, 1);
  CHECK_EQ(kCustomWalkOneCm, 6);
  CHECK_EQ(kCustomJump, 23);
  CHECK_EQ(kCustomDeaths, 32);
  CHECK_EQ(kCustomMobKills, 33);
  CHECK_EQ(kCustomAnimalsBred, 34);
  CHECK_EQ(kCustomPlayerKills, 35);
  CHECK_EQ(kCustomOpenChest, 59);
  CHECK_EQ(kCustomEnchantItem, 55);
  CHECK_EQ(kCustomInteractWithSmithingTable, 76);

  CHECK_STR(std::string(stat_type_name(0)), "mined");
  CHECK_STR(std::string(stat_type_name(8)), "custom");
  CHECK(stat_type_name(9) == nullptr);
  CHECK_STR(std::string(custom_stat_name(kCustomPlayTime)), "minecraft:play_time");
  CHECK_STR(std::string(custom_stat_name(kCustomMobKills)), "minecraft:mob_kills");
  CHECK_STR(std::string(custom_stat_name(kCustomStatCount)), "");
  CHECK_EQ(custom_stat_id("minecraft:play_time"), 1);
  CHECK_EQ(custom_stat_id("play_time"), 1);
  CHECK_EQ(custom_stat_id("minecraft:interact_with_smithing_table"), 76);
  CHECK_EQ(custom_stat_id("minecraft:not_a_stat"), -1);

  // ---- block registry ids, from the state table's order ----
  CHECK_EQ(block_id_of_name("minecraft:air"), 0);
  CHECK_EQ(block_id_of_name("minecraft:stone"), 1);
  CHECK_EQ(block_id_of_name("minecraft:dirt"), 9);
  CHECK_EQ(block_id_of_name("minecraft:cobblestone"), 12);
  CHECK_EQ(block_id_of_name("stone"), 1);
  CHECK_EQ(block_id_of_name("minecraft:not_a_block"), -1);
  CHECK_STR(std::string(block_name_of_id(1)), "minecraft:stone");
  CHECK_STR(std::string(block_name_of_id(-1)), "");
  CHECK_STR(std::string(block_name_of_id(1000000)), "");

  // ---- criterion names, one per value registry ----
  const StatKey mined_stone{kStatMined, block_id_of_name("minecraft:stone")};
  CHECK_STR(stat_criterion_name(mined_stone), "minecraft.mined:minecraft.stone");
  const StatKey play_time{kStatCustom, kCustomPlayTime};
  CHECK_STR(stat_criterion_name(play_time), "minecraft.custom:minecraft.play_time");
  const GeneratedMob* zombie = mob_by_name("minecraft:zombie");
  CHECK(zombie != nullptr);
  const StatKey killed_zombie{kStatKilled, zombie != nullptr ? zombie->entity_type_id : 0};
  CHECK_STR(stat_criterion_name(killed_zombie), "minecraft.killed:minecraft.zombie");
  CHECK(mob_by_entity_type_id(killed_zombie.value) == zombie);
  const std::optional<int32_t> planks = ItemRegistry::instance().id_of("minecraft:oak_planks");
  CHECK(planks.has_value());
  const StatKey crafted_planks{kStatCrafted, *planks};
  CHECK_STR(stat_criterion_name(crafted_planks), "minecraft.crafted:minecraft.oak_planks");

  // Round trips through the criterion parser, for all four registries.
  for (const StatKey& stat : {mined_stone, play_time, crafted_planks}) {
    const std::optional<StatKey> back = stat_from_criterion_name(stat_criterion_name(stat));
    CHECK(back.has_value());
    CHECK(back.has_value() && back->type == stat.type && back->value == stat.value);
  }
  // A type key without its dots also parses (`custom:play_time` — vanilla's
  // `byName` splits at the first colon, so either half may carry its registry
  // key whole; the dotted spelling is the one the criterion name itself uses).
  const std::optional<StatKey> namespaced = stat_from_criterion_name("custom:play_time");
  CHECK(namespaced.has_value());
  CHECK(namespaced.has_value() && namespaced->type == kStatCustom && namespaced->value == 1);
  // The fully-colonized spelling is not one vanilla accepts: it splits at the
  // *first* colon, which would make "minecraft" the type.
  CHECK(!stat_from_criterion_name("minecraft:custom:minecraft.play_time").has_value());
  // Unknown halves are null: a bad type, a bad value.
  CHECK(!stat_from_criterion_name("minecraft.not_a_type:minecraft.stone").has_value());
  CHECK(!stat_from_criterion_name("minecraft.mined:minecraft.not_a_block").has_value());
  CHECK(!stat_from_criterion_name("minecraft.custom:minecraft.not_a_stat").has_value());
  CHECK(!stat_from_criterion_name("dummy").has_value());
  // A stat whose value is a mob the roster does not carry is null too.
  CHECK(!stat_from_criterion_name("minecraft.killed:minecraft:not_a_mob").has_value());
  // A stat whose value name is empty (an id past the registry) renders "".
  CHECK_STR(stat_criterion_name(StatKey{kStatCustom, 9999}), "");
  CHECK_STR(stat_criterion_name(StatKey{kStatTypeCount, 1}), "");

  // ---- the counter ----
  StatsCounter counter;
  CHECK_EQ(counter.value(play_time), 0);
  counter.increment(play_time);
  CHECK_EQ(counter.value(play_time), 1);
  counter.increment(play_time, 41);
  CHECK_EQ(counter.value(play_time), 42);
  counter.set_value(mined_stone, 7);
  CHECK_EQ(counter.value(mined_stone), 7);
  CHECK_EQ(counter.values().size(), 2u);
  counter.set_value(play_time, INT_MAX - 1);
  counter.increment(play_time, 5);
  CHECK_EQ(counter.value(play_time), INT_MAX);
  counter.clear();
  CHECK_EQ(counter.values().size(), 0u);

  if (failures == 0)
    std::printf("stats: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
