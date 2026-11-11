// SPDX-License-Identifier: GPL-3.0-or-later
//
// The entity-type tags the modelled enchantment requirements ask about,
// expanded from data/minecraft/tags/entity_type (nested `#tag` references
// resolved, generated — do not hand-edit). A tag is a list of entity type
// *names*, so this stays correct for a type this engine has no mob row for.
//
// Regenerate with tools/gen_enchantment_table.py.
#pragma once

#include <string_view>

namespace mc::world {

// clang-format off
inline constexpr const char* kArrowsTypes[] = {
    "minecraft:arrow", "minecraft:spectral_arrow",
};
// clang-format on
// clang-format off
inline constexpr const char* kSensitiveToBaneOfArthropodsTypes[] = {
    "minecraft:bee", "minecraft:endermite", "minecraft:silverfish", "minecraft:spider",
    "minecraft:cave_spider",
};
// clang-format on
// clang-format off
inline constexpr const char* kSensitiveToImpalingTypes[] = {
    "minecraft:turtle", "minecraft:axolotl", "minecraft:guardian", "minecraft:elder_guardian",
    "minecraft:cod", "minecraft:pufferfish", "minecraft:salmon", "minecraft:tropical_fish",
    "minecraft:dolphin", "minecraft:squid", "minecraft:glow_squid", "minecraft:tadpole",
    "minecraft:nautilus", "minecraft:zombie_nautilus",
};
// clang-format on
// clang-format off
inline constexpr const char* kSensitiveToSmiteTypes[] = {
    "minecraft:skeleton", "minecraft:stray", "minecraft:wither_skeleton", "minecraft:skeleton_horse",
    "minecraft:bogged", "minecraft:parched", "minecraft:zombie_horse", "minecraft:camel_husk",
    "minecraft:zombie", "minecraft:zombie_villager", "minecraft:zombified_piglin", "minecraft:zoglin",
    "minecraft:drowned", "minecraft:husk", "minecraft:zombie_nautilus", "minecraft:wither",
    "minecraft:phantom",
};
// clang-format on

struct GeneratedEntityTypeTag {
  const char* name; // minecraft:sensitive_to_smite
  const char* const* types;
  int type_count;
};

// clang-format off
inline constexpr GeneratedEntityTypeTag kEntityTypeTags[] = {
    {"minecraft:arrows", kArrowsTypes, 2},
    {"minecraft:sensitive_to_bane_of_arthropods", kSensitiveToBaneOfArthropodsTypes, 5},
    {"minecraft:sensitive_to_impaling", kSensitiveToImpalingTypes, 14},
    {"minecraft:sensitive_to_smite", kSensitiveToSmiteTypes, 17},
};
// clang-format on
inline constexpr int kEntityTypeTagCount = 4;

// EntityTypeTags.test: is `entity_type` (a name, as EntityTypes registers
// them) a member of the tag?
inline bool entity_type_has_tag(std::string_view tag, std::string_view entity_type) {
  for (const GeneratedEntityTypeTag& row : kEntityTypeTags) {
    if (tag != row.name)
      continue;
    for (int i = 0; i < row.type_count; ++i)
      if (entity_type == row.types[i])
        return true;
    return false;
  }
  return false;
}

} // namespace mc::world
