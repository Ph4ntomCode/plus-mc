// SPDX-License-Identifier: GPL-3.0-or-later
//
// Statistics (Phase 4 milestone M): the stat registry and a player's counter.
//
// Vanilla's `Stats` / `Stat` / `StatType` / `StatsCounter` / `ServerStatsCounter`,
// ported as the pure model the session awards through. The two things this
// header exists to get exactly right are both registry *orders*, because the
// wire is made of them:
//
//   * `BuiltInRegistries.STAT_TYPE` is filled in `Stats`'s field order —
//     BLOCK_MINED, ITEM_CRAFTED, ITEM_USED, ITEM_BROKEN, ITEM_PICKED_UP,
//     ITEM_DROPPED, ENTITY_KILLED, ENTITY_KILLED_BY, CUSTOM — so those are the
//     nine `StatType` ids, and `ClientboundAwardStatsPacket` opens every stat
//     with one of them.
//   * `BuiltInRegistries.CUSTOM_STAT` is filled in the same file's field order
//     (leave_game first, interact_with_smithing_table last): 77 entries, ids
//     0..76 (`kCustomStatCount`).
//
// The value half of a stat is that type's own registry id — item, block,
// entity type, or custom-stat id — which is what the client's copy of the same
// registry decodes. `Stat.buildName` turns the pair into the *criterion* name a
// scoreboard objective can use: both halves' colons become dots and the two are
// joined with a colon, which is why a stat criterion reads
// `minecraft.custom:minecraft.play_time` and `minecraft.mined:minecraft.stone`.
//
// Blocks are the one wrinkle: the pack's generated table in block_states.h is a
// *state* table, and a stat wants the `Block` registry id. Vanilla assigns
// state ids in block-registry order (each block's states in one contiguous
// run), so the id this file hands back is the block's rank in
// first-appearance order — verified against the registry's own head in
// tests/stats_test.cpp (air 0, stone 1, dirt 9, cobblestone 12).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mc::world {

// ---- the registry ----------------------------------------------------------

// `Stats`'s StatType fields, in registration order, which is the id the award
// packet writes first.
enum StatTypeId : uint8_t {
  kStatMined = 0,
  kStatCrafted = 1,
  kStatUsed = 2,
  kStatBroken = 3,
  kStatPickedUp = 4,
  kStatDropped = 5,
  kStatKilled = 6,
  kStatKilledBy = 7,
  kStatCustom = 8,
  kStatTypeCount = 9,
};

// `Stats.CUSTOM`'s registry order (the field order of `Stats.java`). Only the
// names the engine awards are commented; the rest exist so the ids line up.
enum CustomStatId : uint16_t {
  kCustomLeaveGame = 0,
  kCustomPlayTime = 1,
  kCustomTotalWorldTime = 2,
  kCustomTimeSinceDeath = 3,
  kCustomTimeSinceRest = 4,
  kCustomSneakTime = 5,
  kCustomWalkOneCm = 6,
  kCustomCrouchOneCm = 7,
  kCustomSprintOneCm = 8,
  kCustomWalkOnWaterOneCm = 9,
  kCustomFallOneCm = 10,
  kCustomClimbOneCm = 11,
  kCustomFlyOneCm = 12,
  kCustomWalkUnderWaterOneCm = 13,
  kCustomMinecartOneCm = 14,
  kCustomBoatOneCm = 15,
  kCustomPigOneCm = 16,
  kCustomHappyGhastOneCm = 17,
  kCustomHorseOneCm = 18,
  kCustomAviateOneCm = 19,
  kCustomSwimOneCm = 20,
  kCustomStriderOneCm = 21,
  kCustomNautilusOneCm = 22,
  kCustomJump = 23,
  kCustomDrop = 24,
  kCustomDamageDealt = 25,
  kCustomDamageDealtAbsorbed = 26,
  kCustomDamageDealtResisted = 27,
  kCustomDamageTaken = 28,
  kCustomDamageBlockedByShield = 29,
  kCustomDamageAbsorbed = 30,
  kCustomDamageResisted = 31,
  kCustomDeaths = 32,
  kCustomMobKills = 33,
  kCustomAnimalsBred = 34,
  kCustomPlayerKills = 35,
  kCustomFishCaught = 36,
  kCustomTalkedToVillager = 37,
  kCustomTradedWithVillager = 38,
  kCustomEatCakeSlice = 39,
  kCustomFillCauldron = 40,
  kCustomUseCauldron = 41,
  kCustomCleanArmor = 42,
  kCustomCleanBanner = 43,
  kCustomCleanShulkerBox = 44,
  kCustomInteractWithBrewingstand = 45,
  kCustomInteractWithBeacon = 46,
  kCustomInspectDropper = 47,
  kCustomInspectHopper = 48,
  kCustomInspectDispenser = 49,
  kCustomPlayNoteblock = 50,
  kCustomTuneNoteblock = 51,
  kCustomPotFlower = 52,
  kCustomTriggerTrappedChest = 53,
  kCustomOpenEnderchest = 54,
  kCustomEnchantItem = 55,
  kCustomPlayRecord = 56,
  kCustomInteractWithFurnace = 57,
  kCustomInteractWithCraftingTable = 58,
  kCustomOpenChest = 59,
  kCustomSleepInBed = 60,
  kCustomOpenShulkerBox = 61,
  kCustomOpenBarrel = 62,
  kCustomInteractWithBlastFurnace = 63,
  kCustomInteractWithSmoker = 64,
  kCustomInteractWithLectern = 65,
  kCustomInteractWithCampfire = 66,
  kCustomInteractWithCartographyTable = 67,
  kCustomInteractWithLoom = 68,
  kCustomInteractWithStonecutter = 69,
  kCustomBellRing = 70,
  kCustomRaidTrigger = 71,
  kCustomRaidWin = 72,
  kCustomInteractWithAnvil = 73,
  kCustomInteractWithGrindstone = 74,
  kCustomTargetHit = 75,
  kCustomInteractWithSmithingTable = 76,
  kCustomStatCount = 77,
};

// One stat: a `StatType` and the value registry's id.
struct StatKey {
  uint8_t type = kStatCustom;
  int32_t value = 0;
  friend bool operator==(const StatKey& a, const StatKey& b) {
    return a.type == b.type && a.value == b.value;
  }
  friend bool operator<(const StatKey& a, const StatKey& b) {
    return a.type != b.type ? a.type < b.type : a.value < b.value;
  }
};

// The value id an item/block/entity name did not resolve to.
constexpr int32_t kUnknownStatValue = -1;

// "mined" / "crafted" / ... (the StatType's own key path), nullptr past the end.
const char* stat_type_name(uint8_t type);

// The custom-stat registry key ("minecraft:play_time"), "" out of range.
std::string_view custom_stat_name(uint16_t id);
// The reverse; -1 for an unknown name. Accepts the vanilla key with or without
// its namespace, and either an id or a criterion-shaped one.
int custom_stat_id(std::string_view name);

// ---- ids of the other registries, as a stat needs them ---------------------

// The `Block` registry id a block name owns, or -1. Derived from the state
// table's first-appearance order (see the header comment); `block_name_of_id`
// is the inverse ("" out of range).
int32_t block_id_of_name(std::string_view name);
std::string_view block_name_of_id(int32_t id);

// ---- criterion names -------------------------------------------------------

// `Stat.buildName`: "<type key>:<value key>" with both keys' colons turned into
// dots. "" when the value is not in its registry.
std::string stat_criterion_name(const StatKey& stat);
// `ObjectiveCriteria.byName`'s stat branch: the inverse of the above.
std::optional<StatKey> stat_from_criterion_name(std::string_view name);

// ---- the counter -----------------------------------------------------------

// `StatsCounter`: a map with a default of 0 and `increment`'s saturation.
// `ServerStatsCounter`'s dirty set and its file are the server's half.
class StatsCounter {
public:
  int value(const StatKey& stat) const;
  void set_value(const StatKey& stat, int count);
  // `increment`: `min(previous + count, Integer.MAX_VALUE)`.
  void increment(const StatKey& stat, int count = 1);
  const std::map<StatKey, int>& values() const {
    return stats_;
  }
  void clear() {
    stats_.clear();
  }

private:
  std::map<StatKey, int> stats_;
};

} // namespace mc::world
