// SPDX-License-Identifier: GPL-3.0-or-later
//
// The armor trim registries (world/trim_table.h, generated) and the two lookups
// that turn a matched `smithing_trim` recipe into a result: which material an
// *addition* stack provides, and which pattern a *template* stack names.
//
// Both are exact rather than fuzzy. `SmithingTrimRecipe.applyTrim` reads the
// material off the addition stack's `minecraft:provides_trim_material`
// component and the pattern off the recipe itself, and this engine's recipe
// table carries no pattern — so the generator pins the recipe data 1:1 (each
// pattern has one trim recipe, whose `template` is exactly that pattern's
// template item, and the recipes' `addition` cell is exactly the material
// items). A recipe that matches therefore can never resolve to a material or a
// pattern the recipe is not about.
#pragma once

#include <cstdint>
#include <string_view>

#include "world/trim_table.h"

namespace mc::world {

// The material a stack *provides* — the value of the addition item's
// `minecraft:provides_trim_material` component — or -1 for an item that
// provides none. `applyTrim` returns an empty result for such an item, so a
// trim with one is a recipe match with no craftable result.
int32_t trim_material_for_item(int32_t item);

// The pattern whose smithing template `item` is, or -1 when the item is not a
// trim template.
int32_t trim_pattern_for_template(int32_t item);

// The two directions the *NBT* form of the trim component needs. Vanilla's
// `ArmorTrim.CODEC` is `{material: <trim_material id>, pattern: <trim_pattern
// id>}` with each holder written as its resource location, so a trimmed piece
// in a container round-trips through these (world/container.cpp). A name is
// "minecraft:gold"; an id this table does not know reads as nullptr / -1.
const char* trim_material_name(int32_t id);
const char* trim_pattern_name(int32_t id);
int32_t trim_material_for_name(std::string_view name);
int32_t trim_pattern_for_name(std::string_view name);

} // namespace mc::world
