// SPDX-License-Identifier: GPL-3.0-or-later
//
// 26.2 entity drops, flattened from the vanilla entity loot tables
// (generated — do not hand-edit).  Regenerate with
// tools/gen_mob_loot.py <loot_table dir> <out>.
//
// One row is one loot pool, under the model documented in the
// generator: a healthy adult mob killed by a player, bare-handed,
// with no enchantments and not on fire.  `chance` is the pool's
// probability and `min_count`/`max_count` its `set_count` range (a
// non-positive roll drops nothing, which is how vanilla encodes
// "0 or 1 spider eye" as a uniform -1..1).  Up to four `item`
// candidates mean vanilla's *one of these* pool: the drop is chosen
// uniformly among them (up to six: the witch's pool).  `rolls_min`/
// `rolls_max` repeat the whole draw (1 for every entity pool but the
// witch's 1..3).
#pragma once

#include <cstdint>

namespace mc::world {

struct GeneratedMobDrop {
  const char* mob;
  const char* item;
  const char* item2;
  const char* item3;
  const char* item4;
  const char* item5;
  const char* item6;
  int8_t min_count;
  int8_t max_count;
  // How many times the pool rolls (1 for every vanilla entity pool but
  // the witch's 1..3); `resolve_mob_drops` repeats the rule this many times.
  uint8_t rolls_min;
  uint8_t rolls_max;
  float chance;
  bool player_kill;
};

// clang-format off
inline constexpr GeneratedMobDrop kMobDrops[] = {
    {"minecraft:blaze", "minecraft:blaze_rod", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:bogged", "minecraft:arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:bogged", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:bogged", "minecraft:tipped_arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:breeze", "minecraft:breeze_rod", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 2, 1, 1, 1.0f, true},
    {"minecraft:camel_husk", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 2, 3, 1, 1, 1.0f, false},
    {"minecraft:cat", "minecraft:string", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:cave_spider", "minecraft:string", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:cave_spider", "minecraft:spider_eye", nullptr, nullptr, nullptr, nullptr, nullptr, -1, 1, 1, 1, 1.0f, true},
    {"minecraft:chicken", "minecraft:feather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:chicken", "minecraft:chicken", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:cod", "minecraft:cod", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:cod", "minecraft:bone_meal", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.05f, false},
    {"minecraft:copper_golem", "minecraft:copper_ingot", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:cow", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:cow", "minecraft:beef", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:creeper", "minecraft:gunpowder", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:dolphin", "minecraft:cod", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:donkey", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:drowned", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:drowned", "minecraft:copper_ingot", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.11f, true},
    {"minecraft:elder_guardian", "minecraft:prismarine_shard", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:elder_guardian", "minecraft:cod", "minecraft:prismarine_crystals", nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:elder_guardian", "minecraft:wet_sponge", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, true},
    {"minecraft:elder_guardian", "minecraft:tide_armor_trim_smithing_template", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:enderman", "minecraft:ender_pearl", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:evoker", "minecraft:totem_of_undying", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:evoker", "minecraft:emerald", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:ghast", "minecraft:ghast_tear", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:ghast", "minecraft:gunpowder", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:glow_squid", "minecraft:glow_ink_sac", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:guardian", "minecraft:prismarine_shard", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:guardian", "minecraft:cod", "minecraft:prismarine_crystals", nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:hoglin", "minecraft:porkchop", nullptr, nullptr, nullptr, nullptr, nullptr, 2, 4, 1, 1, 1.0f, false},
    {"minecraft:hoglin", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:horse", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:husk", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:husk", "minecraft:iron_ingot", "minecraft:carrot", "minecraft:potato", nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.025f, true},
    {"minecraft:iron_golem", "minecraft:poppy", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:iron_golem", "minecraft:iron_ingot", nullptr, nullptr, nullptr, nullptr, nullptr, 3, 5, 1, 1, 1.0f, false},
    {"minecraft:llama", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:mooshroom", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:mooshroom", "minecraft:beef", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:mule", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:nautilus", "minecraft:nautilus_shell", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.05f, true},
    {"minecraft:panda", "minecraft:bamboo", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:parched", "minecraft:arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:parched", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:parched", "minecraft:tipped_arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:parrot", "minecraft:feather", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 2, 1, 1, 1.0f, false},
    {"minecraft:phantom", "minecraft:phantom_membrane", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:pig", "minecraft:porkchop", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:polar_bear", "minecraft:cod", "minecraft:salmon", nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:pufferfish", "minecraft:pufferfish", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:pufferfish", "minecraft:bone_meal", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.05f, false},
    {"minecraft:rabbit", "minecraft:rabbit_hide", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:rabbit", "minecraft:rabbit", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:rabbit", "minecraft:rabbit_foot", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.1f, true},
    {"minecraft:ravager", "minecraft:saddle", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:salmon", "minecraft:salmon", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:salmon", "minecraft:bone_meal", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.05f, false},
    {"minecraft:sheep", "minecraft:mutton", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 2, 1, 1, 1.0f, false},
    {"minecraft:sheep", "minecraft:white_wool", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:shulker", "minecraft:shulker_shell", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.5f, false},
    {"minecraft:skeleton", "minecraft:arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:skeleton", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:skeleton_horse", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:snow_golem", "minecraft:snowball", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 15, 1, 1, 1.0f, false},
    {"minecraft:spider", "minecraft:string", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:spider", "minecraft:spider_eye", nullptr, nullptr, nullptr, nullptr, nullptr, -1, 1, 1, 1, 1.0f, true},
    {"minecraft:squid", "minecraft:ink_sac", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:stray", "minecraft:arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:stray", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:stray", "minecraft:tipped_arrow", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:strider", "minecraft:string", nullptr, nullptr, nullptr, nullptr, nullptr, 2, 5, 1, 1, 1.0f, false},
    {"minecraft:trader_llama", "minecraft:leather", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:tropical_fish", "minecraft:tropical_fish", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:tropical_fish", "minecraft:bone_meal", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.05f, false},
    {"minecraft:turtle", "minecraft:seagrass", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:vindicator", "minecraft:emerald", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, true},
    {"minecraft:warden", "minecraft:sculk_catalyst", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 1.0f, false},
    {"minecraft:witch", "minecraft:glowstone_dust", "minecraft:sugar", "minecraft:spider_eye", "minecraft:glass_bottle", "minecraft:gunpowder", "minecraft:stick", 0, 2, 1, 3, 1.0f, false},
    {"minecraft:witch", "minecraft:redstone", nullptr, nullptr, nullptr, nullptr, nullptr, 4, 8, 1, 1, 1.0f, false},
    {"minecraft:wither_skeleton", "minecraft:coal", nullptr, nullptr, nullptr, nullptr, nullptr, -1, 1, 1, 1, 1.0f, false},
    {"minecraft:wither_skeleton", "minecraft:bone", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:wither_skeleton", "minecraft:wither_skeleton_skull", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.025f, true},
    {"minecraft:zoglin", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 3, 1, 1, 1.0f, false},
    {"minecraft:zombie", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:zombie", "minecraft:iron_ingot", "minecraft:carrot", "minecraft:potato", nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.025f, true},
    {"minecraft:zombie_horse", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 2, 3, 1, 1, 1.0f, false},
    {"minecraft:zombie_nautilus", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 3, 1, 1, 1.0f, true},
    {"minecraft:zombie_villager", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 2, 1, 1, 1.0f, false},
    {"minecraft:zombie_villager", "minecraft:iron_ingot", "minecraft:carrot", "minecraft:potato", nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.025f, true},
    {"minecraft:zombified_piglin", "minecraft:rotten_flesh", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:zombified_piglin", "minecraft:gold_nugget", nullptr, nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 1, 1.0f, false},
    {"minecraft:zombified_piglin", "minecraft:gold_ingot", nullptr, nullptr, nullptr, nullptr, nullptr, 1, 1, 1, 1, 0.025f, true},
};
inline constexpr int kMobDropCount = 96;
// clang-format on

} // namespace mc::world
