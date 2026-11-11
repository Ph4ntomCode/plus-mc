// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// What `Animal.isFood(ItemStack)` answers for the animals this engine
// simulates (137 rows, generated — do not hand-edit), flattened out of
// the vanilla 26.2 item tag data pack by tools/gen_animal_food_table.py. The
// mob list is world/mob_table.h's own `kMobAnimal` rows, so the two tables
// cannot drift; the per-class Java is one line each:
//
//   * minecraft:armadillo              -> #minecraft:armadillo_food
//   * minecraft:axolotl                -> #minecraft:axolotl_food
//   * minecraft:bee                    -> #minecraft:bee_food
//   * minecraft:camel                  -> #minecraft:camel_food
//   * minecraft:camel_husk             -> #minecraft:camel_husk_food
//   * minecraft:cat                    -> #minecraft:cat_food
//   * minecraft:chicken                -> #minecraft:chicken_food
//   * minecraft:cow                    -> #minecraft:cow_food
//   * minecraft:donkey                 -> #minecraft:horse_food
//   * minecraft:fox                    -> #minecraft:fox_food
//   * minecraft:frog                   -> #minecraft:frog_food
//   * minecraft:goat                   -> #minecraft:goat_food
//   * minecraft:happy_ghast            -> #minecraft:happy_ghast_food
//   * minecraft:hoglin                 -> #minecraft:hoglin_food
//   * minecraft:horse                  -> #minecraft:horse_food
//   * minecraft:llama                  -> #minecraft:llama_food
//   * minecraft:mooshroom              -> #minecraft:cow_food
//   * minecraft:mule                   -> #minecraft:horse_food
//   * minecraft:nautilus               -> #minecraft:nautilus_food, #minecraft:nautilus_taming_items
//   * minecraft:ocelot                 -> #minecraft:ocelot_food
//   * minecraft:panda                  -> #minecraft:panda_food
//   * minecraft:pig                    -> #minecraft:pig_food
//   * minecraft:rabbit                 -> #minecraft:rabbit_food
//   * minecraft:sheep                  -> #minecraft:sheep_food
//   * minecraft:skeleton_horse         -> #minecraft:horse_food
//   * minecraft:sniffer                -> #minecraft:sniffer_food
//   * minecraft:strider                -> #minecraft:strider_food
//   * minecraft:trader_llama           -> #minecraft:llama_food
//   * minecraft:turtle                 -> #minecraft:turtle_food
//   * minecraft:wolf                   -> #minecraft:wolf_food
//   * minecraft:zombie_horse           -> #minecraft:zombie_horse_food
//   * minecraft:zombie_nautilus        -> #minecraft:nautilus_food, #minecraft:nautilus_taming_items
//
// Sorted by (mob, item), so `mob_eats` (world/mob.cpp) can walk it in one pass.
//
#pragma once

namespace mc::world {

struct GeneratedAnimalFood {
  const char* mob;
  const char* item;
};

inline constexpr GeneratedAnimalFood kAnimalFoods[] = {
    {"minecraft:armadillo", "minecraft:spider_eye"},
    {"minecraft:axolotl", "minecraft:tropical_fish_bucket"},
    {"minecraft:bee", "minecraft:allium"},
    {"minecraft:bee", "minecraft:azure_bluet"},
    {"minecraft:bee", "minecraft:blue_orchid"},
    {"minecraft:bee", "minecraft:cactus_flower"},
    {"minecraft:bee", "minecraft:cherry_leaves"},
    {"minecraft:bee", "minecraft:chorus_flower"},
    {"minecraft:bee", "minecraft:cornflower"},
    {"minecraft:bee", "minecraft:dandelion"},
    {"minecraft:bee", "minecraft:flowering_azalea"},
    {"minecraft:bee", "minecraft:flowering_azalea_leaves"},
    {"minecraft:bee", "minecraft:lilac"},
    {"minecraft:bee", "minecraft:lily_of_the_valley"},
    {"minecraft:bee", "minecraft:mangrove_propagule"},
    {"minecraft:bee", "minecraft:open_eyeblossom"},
    {"minecraft:bee", "minecraft:orange_tulip"},
    {"minecraft:bee", "minecraft:oxeye_daisy"},
    {"minecraft:bee", "minecraft:peony"},
    {"minecraft:bee", "minecraft:pink_petals"},
    {"minecraft:bee", "minecraft:pink_tulip"},
    {"minecraft:bee", "minecraft:pitcher_plant"},
    {"minecraft:bee", "minecraft:poppy"},
    {"minecraft:bee", "minecraft:red_tulip"},
    {"minecraft:bee", "minecraft:rose_bush"},
    {"minecraft:bee", "minecraft:spore_blossom"},
    {"minecraft:bee", "minecraft:sunflower"},
    {"minecraft:bee", "minecraft:torchflower"},
    {"minecraft:bee", "minecraft:white_tulip"},
    {"minecraft:bee", "minecraft:wildflowers"},
    {"minecraft:bee", "minecraft:wither_rose"},
    {"minecraft:camel", "minecraft:cactus"},
    {"minecraft:camel_husk", "minecraft:rabbit_foot"},
    {"minecraft:cat", "minecraft:cod"},
    {"minecraft:cat", "minecraft:salmon"},
    {"minecraft:chicken", "minecraft:beetroot_seeds"},
    {"minecraft:chicken", "minecraft:melon_seeds"},
    {"minecraft:chicken", "minecraft:pitcher_pod"},
    {"minecraft:chicken", "minecraft:pumpkin_seeds"},
    {"minecraft:chicken", "minecraft:torchflower_seeds"},
    {"minecraft:chicken", "minecraft:wheat_seeds"},
    {"minecraft:cow", "minecraft:wheat"},
    {"minecraft:donkey", "minecraft:apple"},
    {"minecraft:donkey", "minecraft:carrot"},
    {"minecraft:donkey", "minecraft:enchanted_golden_apple"},
    {"minecraft:donkey", "minecraft:golden_apple"},
    {"minecraft:donkey", "minecraft:golden_carrot"},
    {"minecraft:donkey", "minecraft:hay_block"},
    {"minecraft:donkey", "minecraft:sugar"},
    {"minecraft:donkey", "minecraft:wheat"},
    {"minecraft:fox", "minecraft:glow_berries"},
    {"minecraft:fox", "minecraft:sweet_berries"},
    {"minecraft:frog", "minecraft:slime_ball"},
    {"minecraft:goat", "minecraft:wheat"},
    {"minecraft:happy_ghast", "minecraft:snowball"},
    {"minecraft:hoglin", "minecraft:crimson_fungus"},
    {"minecraft:horse", "minecraft:apple"},
    {"minecraft:horse", "minecraft:carrot"},
    {"minecraft:horse", "minecraft:enchanted_golden_apple"},
    {"minecraft:horse", "minecraft:golden_apple"},
    {"minecraft:horse", "minecraft:golden_carrot"},
    {"minecraft:horse", "minecraft:hay_block"},
    {"minecraft:horse", "minecraft:sugar"},
    {"minecraft:horse", "minecraft:wheat"},
    {"minecraft:llama", "minecraft:hay_block"},
    {"minecraft:llama", "minecraft:wheat"},
    {"minecraft:mooshroom", "minecraft:wheat"},
    {"minecraft:mule", "minecraft:apple"},
    {"minecraft:mule", "minecraft:carrot"},
    {"minecraft:mule", "minecraft:enchanted_golden_apple"},
    {"minecraft:mule", "minecraft:golden_apple"},
    {"minecraft:mule", "minecraft:golden_carrot"},
    {"minecraft:mule", "minecraft:hay_block"},
    {"minecraft:mule", "minecraft:sugar"},
    {"minecraft:mule", "minecraft:wheat"},
    {"minecraft:nautilus", "minecraft:cod"},
    {"minecraft:nautilus", "minecraft:cod_bucket"},
    {"minecraft:nautilus", "minecraft:cooked_cod"},
    {"minecraft:nautilus", "minecraft:cooked_salmon"},
    {"minecraft:nautilus", "minecraft:pufferfish"},
    {"minecraft:nautilus", "minecraft:pufferfish_bucket"},
    {"minecraft:nautilus", "minecraft:salmon"},
    {"minecraft:nautilus", "minecraft:salmon_bucket"},
    {"minecraft:nautilus", "minecraft:tropical_fish"},
    {"minecraft:nautilus", "minecraft:tropical_fish_bucket"},
    {"minecraft:ocelot", "minecraft:cod"},
    {"minecraft:ocelot", "minecraft:salmon"},
    {"minecraft:panda", "minecraft:bamboo"},
    {"minecraft:pig", "minecraft:beetroot"},
    {"minecraft:pig", "minecraft:carrot"},
    {"minecraft:pig", "minecraft:potato"},
    {"minecraft:rabbit", "minecraft:carrot"},
    {"minecraft:rabbit", "minecraft:dandelion"},
    {"minecraft:rabbit", "minecraft:golden_carrot"},
    {"minecraft:sheep", "minecraft:wheat"},
    {"minecraft:skeleton_horse", "minecraft:apple"},
    {"minecraft:skeleton_horse", "minecraft:carrot"},
    {"minecraft:skeleton_horse", "minecraft:enchanted_golden_apple"},
    {"minecraft:skeleton_horse", "minecraft:golden_apple"},
    {"minecraft:skeleton_horse", "minecraft:golden_carrot"},
    {"minecraft:skeleton_horse", "minecraft:hay_block"},
    {"minecraft:skeleton_horse", "minecraft:sugar"},
    {"minecraft:skeleton_horse", "minecraft:wheat"},
    {"minecraft:sniffer", "minecraft:torchflower_seeds"},
    {"minecraft:strider", "minecraft:warped_fungus"},
    {"minecraft:trader_llama", "minecraft:hay_block"},
    {"minecraft:trader_llama", "minecraft:wheat"},
    {"minecraft:turtle", "minecraft:seagrass"},
    {"minecraft:wolf", "minecraft:beef"},
    {"minecraft:wolf", "minecraft:chicken"},
    {"minecraft:wolf", "minecraft:cod"},
    {"minecraft:wolf", "minecraft:cooked_beef"},
    {"minecraft:wolf", "minecraft:cooked_chicken"},
    {"minecraft:wolf", "minecraft:cooked_cod"},
    {"minecraft:wolf", "minecraft:cooked_mutton"},
    {"minecraft:wolf", "minecraft:cooked_porkchop"},
    {"minecraft:wolf", "minecraft:cooked_rabbit"},
    {"minecraft:wolf", "minecraft:cooked_salmon"},
    {"minecraft:wolf", "minecraft:mutton"},
    {"minecraft:wolf", "minecraft:porkchop"},
    {"minecraft:wolf", "minecraft:pufferfish"},
    {"minecraft:wolf", "minecraft:rabbit"},
    {"minecraft:wolf", "minecraft:rabbit_stew"},
    {"minecraft:wolf", "minecraft:rotten_flesh"},
    {"minecraft:wolf", "minecraft:salmon"},
    {"minecraft:wolf", "minecraft:tropical_fish"},
    {"minecraft:zombie_horse", "minecraft:red_mushroom"},
    {"minecraft:zombie_nautilus", "minecraft:cod"},
    {"minecraft:zombie_nautilus", "minecraft:cod_bucket"},
    {"minecraft:zombie_nautilus", "minecraft:cooked_cod"},
    {"minecraft:zombie_nautilus", "minecraft:cooked_salmon"},
    {"minecraft:zombie_nautilus", "minecraft:pufferfish"},
    {"minecraft:zombie_nautilus", "minecraft:pufferfish_bucket"},
    {"minecraft:zombie_nautilus", "minecraft:salmon"},
    {"minecraft:zombie_nautilus", "minecraft:salmon_bucket"},
    {"minecraft:zombie_nautilus", "minecraft:tropical_fish"},
    {"minecraft:zombie_nautilus", "minecraft:tropical_fish_bucket"},
};
inline constexpr int kAnimalFoodCount = 137;

} // namespace mc::world

