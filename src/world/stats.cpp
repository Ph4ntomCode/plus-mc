// SPDX-License-Identifier: GPL-3.0-or-later
//
// The statistics model: see world/stats.h for the registry orders this file is
// built on and the decomp lines each table comes from.
#include "world/stats.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "world/block_states.h"
#include "world/item.h"
#include "world/mob_table.h"

namespace mc::world {
namespace {

// `Stats.makeCustomStat` call order (and therefore `BuiltInRegistries.CUSTOM_STAT`
// ids 0..76). Regenerating this list means walking Stats.java's field order.
constexpr const char* kCustomStatNames[kCustomStatCount] = {
    "leave_game",
    "play_time",
    "total_world_time",
    "time_since_death",
    "time_since_rest",
    "sneak_time",
    "walk_one_cm",
    "crouch_one_cm",
    "sprint_one_cm",
    "walk_on_water_one_cm",
    "fall_one_cm",
    "climb_one_cm",
    "fly_one_cm",
    "walk_under_water_one_cm",
    "minecart_one_cm",
    "boat_one_cm",
    "pig_one_cm",
    "happy_ghast_one_cm",
    "horse_one_cm",
    "aviate_one_cm",
    "swim_one_cm",
    "strider_one_cm",
    "nautilus_one_cm",
    "jump",
    "drop",
    "damage_dealt",
    "damage_dealt_absorbed",
    "damage_dealt_resisted",
    "damage_taken",
    "damage_blocked_by_shield",
    "damage_absorbed",
    "damage_resisted",
    "deaths",
    "mob_kills",
    "animals_bred",
    "player_kills",
    "fish_caught",
    "talked_to_villager",
    "traded_with_villager",
    "eat_cake_slice",
    "fill_cauldron",
    "use_cauldron",
    "clean_armor",
    "clean_banner",
    "clean_shulker_box",
    "interact_with_brewingstand",
    "interact_with_beacon",
    "inspect_dropper",
    "inspect_hopper",
    "inspect_dispenser",
    "play_noteblock",
    "tune_noteblock",
    "pot_flower",
    "trigger_trapped_chest",
    "open_enderchest",
    "enchant_item",
    "play_record",
    "interact_with_furnace",
    "interact_with_crafting_table",
    "open_chest",
    "sleep_in_bed",
    "open_shulker_box",
    "open_barrel",
    "interact_with_blast_furnace",
    "interact_with_smoker",
    "interact_with_lectern",
    "interact_with_campfire",
    "interact_with_cartography_table",
    "interact_with_loom",
    "interact_with_stonecutter",
    "bell_ring",
    "raid_trigger",
    "raid_win",
    "interact_with_anvil",
    "interact_with_grindstone",
    "target_hit",
    "interact_with_smithing_table",
};

// `Stats.makeRegistryStatType` order: the registry keys under minecraft:.
constexpr const char* kStatTypeNames[kStatTypeCount] = {
    "mined", "crafted", "used", "broken", "picked_up", "dropped", "killed", "killed_by", "custom",
};

// A namespaced name's path, and the same name whole: "minecraft:stone" ->
// "stone". A name without a colon is already a path. (trade.cpp's `path_of`,
// duplicated rather than shared because these registries are not the same
// ones.)
std::string_view path_of(std::string_view name) {
  const size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

// `Stat.locationToKey`: every colon becomes a dot.
std::string dots_of(std::string_view key) {
  std::string out(key);
  std::replace(out.begin(), out.end(), ':', '.');
  return out;
}

// The value key of a stat as the client's registry spells it: item, block and
// entity names are already namespaced; the custom table's entries are the path.
std::string_view stat_value_key(const StatKey& stat) {
  switch (stat.type) {
  case kStatMined:
    return block_name_of_id(stat.value);
  case kStatKilled:
  case kStatKilledBy: {
    const GeneratedMob* mob = mob_by_entity_type_id(stat.value);
    return mob != nullptr ? std::string_view(mob->name) : std::string_view();
  }
  case kStatCustom:
    return custom_stat_name(static_cast<uint16_t>(stat.value));
  default:
    return ItemRegistry::instance().name_of(stat.value);
  }
}

// `Block` registry ids: the state table is sorted by state id and vanilla
// assigns state ids in block-registry order with each block's states
// contiguous, so a block's id is its rank in first-appearance order. Built
// lazily, once.
const std::vector<std::string_view>& block_names_by_id() {
  static const std::vector<std::string_view> names = [] {
    std::vector<std::string_view> out;
    out.reserve(2048);
    for (const GeneratedBlockState& state : kBlockStates) {
      const std::string_view name(state.name);
      if (out.empty() || out.back() != name) {
        // A block may appear, then another, then the first again only if the
        // registry order assumption is wrong; the table is generated in state
        // order, so a run of equal names is the whole of one block.
        if (std::find(out.begin(), out.end(), name) == out.end())
          out.push_back(name);
      }
    }
    return out;
  }();
  return names;
}

// The custom-stat name without its namespace, for the table lookup.
int custom_stat_id_by_path(std::string_view path) {
  for (uint16_t i = 0; i < kCustomStatCount; ++i)
    if (path == kCustomStatNames[i])
      return static_cast<int>(i);
  return -1;
}

} // namespace

const char* stat_type_name(uint8_t type) {
  return type < kStatTypeCount ? kStatTypeNames[type] : nullptr;
}

std::string_view custom_stat_name(uint16_t id) {
  if (id >= kCustomStatCount)
    return {};
  // The registry key the game registers: "minecraft:" + the field's id.
  static const std::array<std::string, kCustomStatCount> keys = [] {
    std::array<std::string, kCustomStatCount> out;
    for (uint16_t i = 0; i < kCustomStatCount; ++i)
      out[i] = std::string("minecraft:") + kCustomStatNames[i];
    return out;
  }();
  return keys[id];
}

int custom_stat_id(std::string_view name) {
  return custom_stat_id_by_path(path_of(name));
}

int32_t block_id_of_name(std::string_view name) {
  const std::string_view path = path_of(name);
  const std::vector<std::string_view>& names = block_names_by_id();
  for (size_t i = 0; i < names.size(); ++i) {
    if (path_of(names[i]) == path)
      return static_cast<int32_t>(i);
  }
  return kUnknownStatValue;
}

std::string_view block_name_of_id(int32_t id) {
  const std::vector<std::string_view>& names = block_names_by_id();
  if (id < 0 || static_cast<size_t>(id) >= names.size())
    return {};
  return names[static_cast<size_t>(id)];
}

std::string stat_criterion_name(const StatKey& stat) {
  const char* type = stat_type_name(stat.type);
  if (type == nullptr)
    return {};
  const std::string_view value = stat_value_key(stat);
  if (value.empty())
    return {};
  // Both halves keep their registry keys, dots on both sides of the joining
  // colon: "minecraft.custom" + ":" + "minecraft.play_time".
  return dots_of(std::string("minecraft:") + type) + ":" + dots_of(value);
}

std::optional<StatKey> stat_from_criterion_name(std::string_view name) {
  const size_t colon = name.find(':');
  if (colon == std::string_view::npos)
    return std::nullopt;
  const std::string_view type_part = name.substr(0, colon);
  const std::string_view value_part = name.substr(colon + 1);
  if (value_part.empty())
    return std::nullopt;
  // The canonical spelling is dotted on both sides ("minecraft.mined"), but the
  // registry keys themselves are also accepted ("minecraft:mined"): those have
  // no dot in the type half.
  const auto un_dot = [](std::string_view part) -> std::string {
    std::string out(part);
    std::replace(out.begin(), out.end(), '.', ':');
    return out;
  };
  const std::string type_key =
      type_part.find('.') != std::string_view::npos ? un_dot(type_part) : std::string(type_part);
  const std::string_view type_path = path_of(type_key);
  const std::string value_key =
      value_part.find('.') != std::string_view::npos ? un_dot(value_part) : std::string(value_part);

  for (uint8_t type = 0; type < kStatTypeCount; ++type) {
    if (type_path != kStatTypeNames[type])
      continue;
    StatKey stat;
    stat.type = type;
    if (type == kStatCustom) {
      const int id = custom_stat_id(value_key);
      if (id < 0)
        return std::nullopt;
      stat.value = id;
      return stat;
    }
    if (type == kStatMined) {
      const int32_t id = block_id_of_name(value_key);
      if (id < 0)
        return std::nullopt;
      stat.value = id;
      return stat;
    }
    if (type == kStatKilled || type == kStatKilledBy) {
      const GeneratedMob* mob = mob_by_name(value_key);
      if (mob == nullptr)
        return std::nullopt;
      stat.value = mob->entity_type_id;
      return stat;
    }
    const std::optional<int32_t> id = ItemRegistry::instance().id_of(value_key);
    if (!id.has_value())
      return std::nullopt;
    stat.value = *id;
    return stat;
  }
  return std::nullopt;
}

int StatsCounter::value(const StatKey& stat) const {
  const auto it = stats_.find(stat);
  return it == stats_.end() ? 0 : it->second;
}

void StatsCounter::set_value(const StatKey& stat, int count) {
  stats_[stat] = count;
}

void StatsCounter::increment(const StatKey& stat, int count) {
  const long long next = static_cast<long long>(value(stat)) + static_cast<long long>(count);
  stats_[stat] = static_cast<int>(std::min<long long>(next, 2147483647LL));
}

} // namespace mc::world
