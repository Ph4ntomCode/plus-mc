// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 40 26.2 minecraft:attribute registry ids (generated — do not
// hand-edit).
//
// Source: reports/registries.json (RegistryDumpReport). Unlike damage types
// this registry is *static*: it is registered in code and the client is never
// sent it, so the id in an update_attributes AttributeSnapshot is this index
// and nothing else. Regenerate with tools/gen_attribute_table.py.
#pragma once

namespace mc::world {

struct GeneratedAttribute {
  const char* name; // minecraft:max_health
  int32_t id;       // registry index == the wire id
};

// clang-format off
inline constexpr GeneratedAttribute kAttributes[] = {
    {"minecraft:air_drag_modifier", 0},
    {"minecraft:armor", 1},
    {"minecraft:armor_toughness", 2},
    {"minecraft:attack_damage", 3},
    {"minecraft:attack_knockback", 4},
    {"minecraft:attack_speed", 5},
    {"minecraft:below_name_distance", 6},
    {"minecraft:block_break_speed", 7},
    {"minecraft:block_interaction_range", 8},
    {"minecraft:bounciness", 9},
    {"minecraft:burning_time", 10},
    {"minecraft:camera_distance", 11},
    {"minecraft:explosion_knockback_resistance", 12},
    {"minecraft:entity_interaction_range", 13},
    {"minecraft:fall_damage_multiplier", 14},
    {"minecraft:flying_speed", 15},
    {"minecraft:follow_range", 16},
    {"minecraft:friction_modifier", 17},
    {"minecraft:gravity", 18},
    {"minecraft:jump_strength", 19},
    {"minecraft:knockback_resistance", 20},
    {"minecraft:luck", 21},
    {"minecraft:max_absorption", 22},
    {"minecraft:max_health", 23},
    {"minecraft:mining_efficiency", 24},
    {"minecraft:movement_efficiency", 25},
    {"minecraft:movement_speed", 26},
    {"minecraft:name_tag_distance", 27},
    {"minecraft:oxygen_bonus", 28},
    {"minecraft:safe_fall_distance", 29},
    {"minecraft:scale", 30},
    {"minecraft:sneaking_speed", 31},
    {"minecraft:spawn_reinforcements", 32},
    {"minecraft:step_height", 33},
    {"minecraft:submerged_mining_speed", 34},
    {"minecraft:sweeping_damage_ratio", 35},
    {"minecraft:tempt_range", 36},
    {"minecraft:water_movement_efficiency", 37},
    {"minecraft:waypoint_transmit_range", 38},
    {"minecraft:waypoint_receive_range", 39},
};
inline constexpr int kAttributeCount = 40;
// clang-format on

} // namespace mc::world
