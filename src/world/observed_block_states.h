// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
//
// Block states observed in a live vanilla 26.2 capture (146 states), re-pinned
// to the exact clientbound network ids in the 26.3 server's own data-generator
// report (reports/blocks.json). The capture came from cell-level alignment;
// the ids come from BlockListReport. Regenerate, do not hand-edit.
//
#pragma once

#include <cstdint>

#include <utility>

namespace mc::world {
// {name, canonical sorted properties string} -> network state id.
inline const std::pair<const char*, const char*> kObservedBlockStates[] = {
    {"minecraft:air", ""},  // 0
    {"minecraft:stone", ""},  // 1
    {"minecraft:granite", ""},  // 2
    {"minecraft:diorite", ""},  // 4
    {"minecraft:andesite", ""},  // 6
    {"minecraft:grass_block", "snowy=false"},  // 9
    {"minecraft:dirt", ""},  // 10
    {"minecraft:bedrock", ""},  // 88
    {"minecraft:water", "level=0"},  // 89
    {"minecraft:water", "level=1"},  // 90
    {"minecraft:water", "level=8"},  // 97
    {"minecraft:lava", "level=0"},  // 105
    {"minecraft:lava", "level=2"},  // 107
    {"minecraft:lava", "level=4"},  // 109
    {"minecraft:lava", "level=6"},  // 111
    {"minecraft:lava", "level=8"},  // 113
    {"minecraft:sand", ""},  // 121
    {"minecraft:gravel", ""},  // 127
    {"minecraft:gold_ore", ""},  // 132
    {"minecraft:deepslate_gold_ore", ""},  // 133
    {"minecraft:iron_ore", ""},  // 134
    {"minecraft:deepslate_iron_ore", ""},  // 135
    {"minecraft:coal_ore", ""},  // 136
    {"minecraft:oak_log", "axis=x"},  // 139
    {"minecraft:oak_log", "axis=y"},  // 140
    {"minecraft:oak_log", "axis=z"},  // 141
    {"minecraft:birch_log", "axis=y"},  // 146
    {"minecraft:dark_oak_log", "axis=y"},  // 158
    {"minecraft:oak_leaves", "distance=1,persistent=false,waterlogged=false"},  // 270
    {"minecraft:oak_leaves", "distance=2,persistent=false,waterlogged=false"},  // 274
    {"minecraft:oak_leaves", "distance=3,persistent=false,waterlogged=false"},  // 278
    {"minecraft:oak_leaves", "distance=4,persistent=false,waterlogged=false"},  // 282
    {"minecraft:oak_leaves", "distance=5,persistent=false,waterlogged=false"},  // 286
    {"minecraft:oak_leaves", "distance=6,persistent=false,waterlogged=false"},  // 290
    {"minecraft:birch_leaves", "distance=1,persistent=false,waterlogged=false"},  // 326
    {"minecraft:birch_leaves", "distance=2,persistent=false,waterlogged=false"},  // 330
    {"minecraft:birch_leaves", "distance=3,persistent=false,waterlogged=false"},  // 334
    {"minecraft:birch_leaves", "distance=4,persistent=false,waterlogged=false"},  // 338
    {"minecraft:dark_oak_leaves", "distance=1,persistent=false,waterlogged=false"},  // 438
    {"minecraft:dark_oak_leaves", "distance=2,persistent=false,waterlogged=false"},  // 442
    {"minecraft:dark_oak_leaves", "distance=3,persistent=false,waterlogged=false"},  // 446
    {"minecraft:dark_oak_leaves", "distance=4,persistent=false,waterlogged=false"},  // 450
    {"minecraft:dark_oak_leaves", "distance=5,persistent=false,waterlogged=false"},  // 454
    {"minecraft:lapis_ore", ""},  // 662
    {"minecraft:deepslate_lapis_ore", ""},  // 663
    {"minecraft:short_grass", ""},  // 2363
    {"minecraft:deepslate_diamond_ore", ""},  // 6864
    {"minecraft:redstone_ore", "lit=false"},  // 8552
    {"minecraft:deepslate_redstone_ore", "lit=false"},  // 8554
    {"minecraft:clay", ""},  // 8616
    {"minecraft:red_mushroom_block", "down=false,east=true,north=true,south=false,up=true,west=false"},  // 9601
    {"minecraft:red_mushroom_block", "down=false,east=true,north=true,south=false,up=false,west=false"},  // 9603
    {"minecraft:red_mushroom_block", "down=false,east=true,north=false,south=true,up=true,west=false"},  // 9605
    {"minecraft:red_mushroom_block", "down=false,east=true,north=false,south=true,up=false,west=false"},  // 9607
    {"minecraft:red_mushroom_block", "down=false,east=true,north=false,south=false,up=true,west=false"},  // 9609
    {"minecraft:red_mushroom_block", "down=false,east=true,north=false,south=false,up=false,west=false"},  // 9611
    {"minecraft:red_mushroom_block", "down=false,east=false,north=true,south=false,up=true,west=true"},  // 9616
    {"minecraft:red_mushroom_block", "down=false,east=false,north=true,south=false,up=true,west=false"},  // 9617
    {"minecraft:red_mushroom_block", "down=false,east=false,north=true,south=false,up=false,west=true"},  // 9618
    {"minecraft:red_mushroom_block", "down=false,east=false,north=true,south=false,up=false,west=false"},  // 9619
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=true,up=true,west=true"},  // 9620
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=true,up=true,west=false"},  // 9621
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=true,up=false,west=true"},  // 9622
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=true,up=false,west=false"},  // 9623
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=false,up=true,west=true"},  // 9624
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=false,up=true,west=false"},  // 9625
    {"minecraft:red_mushroom_block", "down=false,east=false,north=false,south=false,up=false,west=true"},  // 9626
    {"minecraft:mushroom_stem", "down=false,east=true,north=true,south=true,up=false,west=true"},  // 9662
    {"minecraft:vine", "east=true,north=false,south=false,up=false,west=false"},  // 10107
    {"minecraft:vine", "east=false,north=true,south=false,up=false,west=false"},  // 10115
    {"minecraft:vine", "east=false,north=false,south=true,up=false,west=false"},  // 10119
    {"minecraft:vine", "east=false,north=false,south=false,up=true,west=false"},  // 10121
    {"minecraft:glow_lichen", "down=true,east=false,north=false,south=false,up=false,waterlogged=false,west=false"},  // 10187
    {"minecraft:glow_lichen", "down=false,east=true,north=true,south=false,up=false,waterlogged=false,west=false"},  // 10203
    {"minecraft:glow_lichen", "down=false,east=false,north=true,south=false,up=true,waterlogged=false,west=false"},  // 10231
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=true,up=false,waterlogged=true,west=false"},  // 10241
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=true,up=false,waterlogged=false,west=false"},  // 10243
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=false,up=true,waterlogged=true,west=true"},  // 10244
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=false,up=true,waterlogged=true,west=false"},  // 10245
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=false,up=false,waterlogged=true,west=true"},  // 10248
    {"minecraft:glow_lichen", "down=false,east=false,north=false,south=false,up=false,waterlogged=false,west=true"},  // 10250
    {"minecraft:peony", "half=upper"},  // 14768
    {"minecraft:peony", "half=lower"},  // 14769
    {"minecraft:tall_grass", "half=upper"},  // 14770
    {"minecraft:tall_grass", "half=lower"},  // 14771
    {"minecraft:amethyst_block", ""},  // 26759
    {"minecraft:budding_amethyst", ""},  // 26760
    {"minecraft:amethyst_cluster", "facing=north,waterlogged=false"},  // 26762
    {"minecraft:large_amethyst_bud", "facing=down,waterlogged=false"},  // 26784
    {"minecraft:medium_amethyst_bud", "facing=south,waterlogged=false"},  // 26790
    {"minecraft:small_amethyst_bud", "facing=north,waterlogged=false"},  // 26798
    {"minecraft:tuff", ""},  // 26809
    {"minecraft:calcite", ""},  // 30517
    {"minecraft:copper_ore", ""},  // 31147
    {"minecraft:deepslate_copper_ore", ""},  // 31148
    {"minecraft:cave_vines", "age=23,berries=true"},  // 33652
    {"minecraft:cave_vines", "age=23,berries=false"},  // 33653
    {"minecraft:cave_vines", "age=24,berries=true"},  // 33654
    {"minecraft:cave_vines", "age=24,berries=false"},  // 33655
    {"minecraft:cave_vines", "age=25,berries=true"},  // 33656
    {"minecraft:cave_vines", "age=25,berries=false"},  // 33657
    {"minecraft:cave_vines_plant", "berries=true"},  // 33658
    {"minecraft:cave_vines_plant", "berries=false"},  // 33659
    {"minecraft:spore_blossom", ""},  // 33660
    {"minecraft:azalea", ""},  // 33661
    {"minecraft:flowering_azalea", ""},  // 33662
    {"minecraft:moss_carpet", ""},  // 33663
    {"minecraft:leaf_litter", "facing=north,segment_amount=1"},  // 33696
    {"minecraft:leaf_litter", "facing=north,segment_amount=2"},  // 33697
    {"minecraft:leaf_litter", "facing=north,segment_amount=3"},  // 33698
    {"minecraft:leaf_litter", "facing=north,segment_amount=4"},  // 33699
    {"minecraft:leaf_litter", "facing=south,segment_amount=1"},  // 33700
    {"minecraft:leaf_litter", "facing=south,segment_amount=2"},  // 33701
    {"minecraft:leaf_litter", "facing=south,segment_amount=3"},  // 33702
    {"minecraft:leaf_litter", "facing=south,segment_amount=4"},  // 33703
    {"minecraft:leaf_litter", "facing=west,segment_amount=1"},  // 33704
    {"minecraft:leaf_litter", "facing=west,segment_amount=2"},  // 33705
    {"minecraft:leaf_litter", "facing=west,segment_amount=3"},  // 33706
    {"minecraft:leaf_litter", "facing=west,segment_amount=4"},  // 33707
    {"minecraft:leaf_litter", "facing=east,segment_amount=1"},  // 33708
    {"minecraft:leaf_litter", "facing=east,segment_amount=2"},  // 33709
    {"minecraft:leaf_litter", "facing=east,segment_amount=3"},  // 33710
    {"minecraft:leaf_litter", "facing=east,segment_amount=4"},  // 33711
    {"minecraft:moss_block", ""},  // 33712
    {"minecraft:big_dripleaf", "facing=north,tilt=none,waterlogged=true"},  // 33713
    {"minecraft:big_dripleaf", "facing=north,tilt=none,waterlogged=false"},  // 33714
    {"minecraft:big_dripleaf", "facing=south,tilt=none,waterlogged=true"},  // 33721
    {"minecraft:big_dripleaf", "facing=south,tilt=none,waterlogged=false"},  // 33722
    {"minecraft:big_dripleaf", "facing=west,tilt=none,waterlogged=true"},  // 33729
    {"minecraft:big_dripleaf", "facing=west,tilt=none,waterlogged=false"},  // 33730
    {"minecraft:big_dripleaf", "facing=east,tilt=none,waterlogged=false"},  // 33738
    {"minecraft:big_dripleaf_stem", "facing=north,waterlogged=true"},  // 33745
    {"minecraft:big_dripleaf_stem", "facing=south,waterlogged=true"},  // 33747
    {"minecraft:big_dripleaf_stem", "facing=south,waterlogged=false"},  // 33748
    {"minecraft:big_dripleaf_stem", "facing=west,waterlogged=true"},  // 33749
    {"minecraft:big_dripleaf_stem", "facing=west,waterlogged=false"},  // 33750
    {"minecraft:big_dripleaf_stem", "facing=east,waterlogged=true"},  // 33751
    {"minecraft:big_dripleaf_stem", "facing=east,waterlogged=false"},  // 33752
    {"minecraft:small_dripleaf", "facing=north,half=upper,waterlogged=false"},  // 33754
    {"minecraft:small_dripleaf", "facing=north,half=lower,waterlogged=true"},  // 33755
    {"minecraft:small_dripleaf", "facing=west,half=upper,waterlogged=false"},  // 33762
    {"minecraft:small_dripleaf", "facing=west,half=lower,waterlogged=true"},  // 33763
    {"minecraft:small_dripleaf", "facing=east,half=upper,waterlogged=false"},  // 33766
    {"minecraft:small_dripleaf", "facing=east,half=lower,waterlogged=true"},  // 33767
    {"minecraft:deepslate", "axis=y"},  // 33774
    {"minecraft:smooth_basalt", ""},  // 35426
};
inline const int32_t kObservedBlockStateIds[] = {
    0,
    1,
    2,
    4,
    6,
    9,
    10,
    88,
    89,
    90,
    97,
    105,
    107,
    109,
    111,
    113,
    121,
    127,
    132,
    133,
    134,
    135,
    136,
    139,
    140,
    141,
    146,
    158,
    270,
    274,
    278,
    282,
    286,
    290,
    326,
    330,
    334,
    338,
    438,
    442,
    446,
    450,
    454,
    662,
    663,
    2363,
    6864,
    8552,
    8554,
    8616,
    9601,
    9603,
    9605,
    9607,
    9609,
    9611,
    9616,
    9617,
    9618,
    9619,
    9620,
    9621,
    9622,
    9623,
    9624,
    9625,
    9626,
    9662,
    10107,
    10115,
    10119,
    10121,
    10187,
    10203,
    10231,
    10241,
    10243,
    10244,
    10245,
    10248,
    10250,
    14768,
    14769,
    14770,
    14771,
    26759,
    26760,
    26762,
    26784,
    26790,
    26798,
    26809,
    30517,
    31147,
    31148,
    33652,
    33653,
    33654,
    33655,
    33656,
    33657,
    33658,
    33659,
    33660,
    33661,
    33662,
    33663,
    33696,
    33697,
    33698,
    33699,
    33700,
    33701,
    33702,
    33703,
    33704,
    33705,
    33706,
    33707,
    33708,
    33709,
    33710,
    33711,
    33712,
    33713,
    33714,
    33721,
    33722,
    33729,
    33730,
    33738,
    33745,
    33747,
    33748,
    33749,
    33750,
    33751,
    33752,
    33754,
    33755,
    33762,
    33763,
    33766,
    33767,
    33774,
    35426,
};
inline constexpr int kObservedBlockStateCount = 146;

} // namespace mc::world
