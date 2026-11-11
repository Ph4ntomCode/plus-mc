// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 26.2 armor trim registries (generated — do not hand-edit): the
// 11 minecraft:trim_material entries, the 18 minecraft:trim_pattern
// entries, and the two links the smithing table needs to apply a trim.
//
// Both registries are *datapack* registries, so their order comes from the
// client's own registry order (clientconfig/registries/trim_material.txt and
// trim_pattern.txt), cross-checked against the data tree and the trim
// recipes. The `minecraft:trim` item component (data component type id
// 58) stores a Holder<TrimMaterial> and a
// Holder<TrimPattern>, and `ByteBufCodecs.holder` writes each as `id + 1`
// (0 is a direct holder, which this engine never writes).
//
// The pattern -> template link is a *recipe* in 26.2: `TrimPattern` is
// `(asset_id, description, decal)` and carries no template item, so the
// mapping here is resolved from the 18 `smithing_trim` recipes and pinned
// 1:1 by the generator — each pattern has exactly one recipe, its `template`
// names exactly one item, and no two patterns share a template. That is what
// lets the matcher resolve a matched trim recipe's pattern from the template
// stack in the player's slot.
//
// The material -> item link is the item's `minecraft:provides_trim_material`
// component (set in Items.java, so it exists in no data file); the generator
// verifies that exactly these items are `#minecraft:trim_materials`, which is
// the `addition` cell of every trim recipe, so an item the matcher accepts
// there always resolves to the material vanilla would use.
//
// Regenerate with tools/gen_trim_table.py --reports <reports dir>.
#pragma once

#include <cstdint>

namespace mc::world {

struct GeneratedTrimMaterial {
  const char* name; // "minecraft:quartz"
  int32_t id;       // the trim_material registry id; the wire carries id + 1
  int32_t item;     // the item whose provides_trim_material component names it
};

struct GeneratedTrimPattern {
  const char* name;      // "minecraft:bolt"
  int32_t id;            // the trim_pattern registry id; the wire carries id + 1
  int32_t template_item; // the pattern's smithing template item
};

// clang-format off
inline constexpr GeneratedTrimMaterial kTrimMaterials[] = {
    {"minecraft:amethyst", 0, 1016},
    {"minecraft:copper", 1, 1020},
    {"minecraft:diamond", 2, 1012},
    {"minecraft:emerald", 3, 1013},
    {"minecraft:gold", 4, 1022},
    {"minecraft:iron", 5, 1018},
    {"minecraft:lapis", 6, 1014},
    {"minecraft:netherite", 7, 1023},
    {"minecraft:quartz", 8, 1015},
    {"minecraft:redstone", 9, 824},
    {"minecraft:resin", 10, 1397},
};
inline constexpr GeneratedTrimPattern kTrimPatterns[] = {
    {"minecraft:bolt", 0, 1597},
    {"minecraft:coast", 1, 1582},
    {"minecraft:dune", 2, 1581},
    {"minecraft:eye", 3, 1585},
    {"minecraft:flow", 4, 1596},
    {"minecraft:host", 5, 1595},
    {"minecraft:raiser", 6, 1594},
    {"minecraft:rib", 7, 1589},
    {"minecraft:sentry", 8, 1580},
    {"minecraft:shaper", 9, 1592},
    {"minecraft:silence", 10, 1593},
    {"minecraft:snout", 11, 1588},
    {"minecraft:spire", 12, 1590},
    {"minecraft:tide", 13, 1587},
    {"minecraft:vex", 14, 1586},
    {"minecraft:ward", 15, 1584},
    {"minecraft:wayfinder", 16, 1591},
    {"minecraft:wild", 17, 1583},
};
// clang-format on
inline constexpr uint32_t kTrimMaterialCount = 11;
inline constexpr uint32_t kTrimPatternCount = 18;

} // namespace mc::world
