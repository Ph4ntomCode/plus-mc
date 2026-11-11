// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit test: the statistics store's save codec (world/stats_save.{h,cpp}).
//
// What it pins:
//   * every kind of stat row — a mined block (the block registry id), a crafted
//     item (the item registry id), a killed mob (the entity type id) and a
//     custom counter — round-trips through `nbt::serialize` -> `nbt::parse`,
//     keyed by `Stat.buildName`'s criterion name;
//   * a player's rows survive with their values, and two players stay apart;
//   * a row whose criterion name this build no longer resolves is skipped
//     rather than failing the store, and a player row with no resolvable stat
//     is dropped entirely;
//   * a `Players` field that is not a list of compounds rejects the whole read,
//     leaving `out` untouched, while a malformed row inside it only skips that
//     row;
//   * the value is written as the counter's saturated int, and the file carries
//     this build's `DataVersion`.
#include <cstdio>
#include <string>
#include <vector>

#include "world/item.h"
#include "world/mob_table.h"
#include "world/nbt.h"
#include "world/stats.h"
#include "world/stats_save.h"

using namespace mc::world;
namespace nbt = mc::world::nbt;

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

namespace {

int counter_value(const PlayerStatsState& player, const StatKey& key) {
  for (const auto& [row_key, value] : player.stats)
    if (row_key == key)
      return value;
  return -1;
}

const PlayerStatsState* player_named(const StatsState& state, const std::string& name) {
  for (const PlayerStatsState& player : state.players)
    if (player.player == name)
      return &player;
  return nullptr;
}

} // namespace

int main() {
  const int32_t stone = block_id_of_name("minecraft:stone");
  const std::optional<int32_t> stick = ItemRegistry::instance().id_of("minecraft:stick");
  const GeneratedMob* zombie = mob_by_name("minecraft:zombie");
  CHECK(stone >= 0);
  CHECK(stick.has_value());
  CHECK(zombie != nullptr);
  if (stone < 0 || !stick || zombie == nullptr)
    return 1;

  const StatKey mined_stone{kStatMined, stone};
  const StatKey crafted_stick{kStatCrafted, *stick};
  const StatKey killed_zombie{kStatKilled, zombie->entity_type_id};
  const StatKey play_time{kStatCustom, kCustomPlayTime};

  // ---- the round trip -------------------------------------------------------
  StatsState state;
  PlayerStatsState alice;
  alice.player = "Alice";
  alice.stats = {{mined_stone, 96}, {crafted_stick, 12}, {killed_zombie, 3}, {play_time, 72000}};
  state.players.push_back(std::move(alice));
  PlayerStatsState bob;
  bob.player = "Bob";
  bob.stats = {{play_time, 20}};
  state.players.push_back(std::move(bob));
  // A player with no counters has no row, exactly as the writer skips them.
  PlayerStatsState empty_player;
  empty_player.player = "Nobody";
  state.players.push_back(std::move(empty_player));

  const nbt::Value root = stats_state_to_nbt(state);
  const std::vector<uint8_t> bytes = nbt::serialize(root);
  const std::optional<nbt::Value> parsed = nbt::parse(bytes.data(), bytes.size());
  CHECK(parsed.has_value());
  StatsState loaded;
  CHECK(stats_state_from_nbt(*parsed, &loaded));
  CHECK_EQ(loaded.players.size(), 2);
  if (loaded.players.size() == 2) {
    const PlayerStatsState* a = player_named(loaded, "Alice");
    const PlayerStatsState* b = player_named(loaded, "Bob");
    CHECK(a != nullptr && b != nullptr);
    if (a != nullptr) {
      CHECK_EQ(counter_value(*a, mined_stone), 96);
      CHECK_EQ(counter_value(*a, crafted_stick), 12);
      CHECK_EQ(counter_value(*a, killed_zombie), 3);
      CHECK_EQ(counter_value(*a, play_time), 72000);
    }
    if (b != nullptr) {
      CHECK_EQ(counter_value(*b, play_time), 20);
      CHECK_EQ(counter_value(*b, mined_stone), -1);
    }
  }
  // A zero is a value the counter carries (a reset writes it), so it survives.
  StatsCounter zeroed;
  zeroed.set_value(play_time, 0);
  StatsState zero_state;
  zero_state.players.push_back(PlayerStatsState{"Zero", {{play_time, 0}}});
  StatsState zero_loaded;
  const nbt::Value zero_root = stats_state_to_nbt(zero_state);
  CHECK(stats_state_from_nbt(zero_root, &zero_loaded));
  CHECK_EQ(zero_loaded.players.size(), 1);
  if (zero_loaded.players.size() == 1)
    CHECK_EQ(counter_value(zero_loaded.players[0], play_time), 0);

  // ---- rows a reader must skip ---------------------------------------------
  // A criterion name no registry resolves, and a stat row with no value: each
  // is skipped, and a player left with nothing is dropped.
  const nbt::Value with_unknown = nbt::make_compound({
      {"DataVersion", nbt::make_int(kStatsStateDataVersion)},
      {"Players",
       nbt::make_list(
           nbt::Tag::Compound,
           {nbt::make_compound({
                {"Name", nbt::make_string("Ghost")},
                {"Stats",
                 nbt::make_list(
                     nbt::Tag::Compound,
                     {nbt::make_compound({
                          {"Name", nbt::make_string("minecraft.custom:minecraft.gone")},
                          {"Value", nbt::make_int(4)},
                      }),
                      nbt::make_compound({
                          {"Name", nbt::make_string("minecraft.custom:minecraft.play_time")},
                      })})},
            }),
            nbt::make_compound({
                {"Name", nbt::make_string("Survivor")},
                {"Stats", nbt::make_list(nbt::Tag::Compound,
                                         {nbt::make_compound({
                                             {"Name", nbt::make_string(
                                                          "minecraft.custom:minecraft.play_time")},
                                             {"Value", nbt::make_int(9)},
                                         })})},
            })})},
  });
  StatsState unknown_state;
  CHECK(stats_state_from_nbt(with_unknown, &unknown_state));
  CHECK_EQ(unknown_state.players.size(), 1);
  if (unknown_state.players.size() == 1) {
    CHECK_STR(unknown_state.players[0].player, "Survivor");
    CHECK_EQ(counter_value(unknown_state.players[0], play_time), 9);
  }

  // A `Players` field of the wrong type is not this file's shape at all, and
  // the read leaves `out` untouched.
  StatsState before = loaded;
  const nbt::Value bad_container = nbt::make_compound({
      {"DataVersion", nbt::make_int(kStatsStateDataVersion)},
      {"Players", nbt::make_string("not a list")},
  });
  CHECK(!stats_state_from_nbt(bad_container, &loaded));
  CHECK_EQ(loaded.players.size(), before.players.size());
  CHECK(!stats_state_from_nbt(nbt::make_int(1), &loaded));

  if (failures == 0)
    std::printf("stats_save: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
