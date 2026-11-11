// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 51 26.2 vanilla damage types, in wire order (generated — do not
// hand-edit).
//
// `id` is the index this type has in the minecraft:damage_type registry, which
// is a *synchronized* registry: the client is sent the same order during the
// configuration phase (clientconfig/registries/damage_type.txt), so the id is
// both a wire holder id (damage_event) and the lookup key here.
//
// `tags` is a bitmask over the minecraft:damage_type tags — the vanilla damage
// pipeline gates on tags, not on names (getDamageAfterArmorAbsorb checks
// BYPASSES_ARMOR, is_fire gates igniting, and so on). Sixty-four bits, not
// thirty-two: vanilla ships 34 tags and `1u << 32` would be undefined.
//
// Regenerate with tools/gen_damage_table.py.
#pragma once

#include <cstdint>

namespace mc::world {

// DamageScaling: whether the (difficulty-scaled) damage amount applies.
constexpr uint8_t kDamageScalingNever = 0;
constexpr uint8_t kDamageScalingWhenCausedByLivingNonPlayer = 1;
constexpr uint8_t kDamageScalingAlways = 2;

// DeathMessageType: which lang key the death message is built from.
constexpr uint8_t kDeathMessageDefault = 0;
constexpr uint8_t kDeathMessageFallVariants = 1;
constexpr uint8_t kDeathMessageIntentionalGameDesign = 2;

// minecraft:damage_type tag bits, in sorted-tag order.
constexpr uint64_t kDamageTagAlwaysHurtsEnderDragons = 1ull << 0;
constexpr uint64_t kDamageTagAlwaysKillsArmorStands = 1ull << 1;
constexpr uint64_t kDamageTagAlwaysMostSignificantFall = 1ull << 2;
constexpr uint64_t kDamageTagAlwaysTriggersSilverfish = 1ull << 3;
constexpr uint64_t kDamageTagAvoidsGuardianThorns = 1ull << 4;
constexpr uint64_t kDamageTagBurnFromStepping = 1ull << 5;
constexpr uint64_t kDamageTagBurnsArmorStands = 1ull << 6;
constexpr uint64_t kDamageTagBypassesArmor = 1ull << 7;
constexpr uint64_t kDamageTagBypassesCooldown = 1ull << 8;
constexpr uint64_t kDamageTagBypassesEffects = 1ull << 9;
constexpr uint64_t kDamageTagBypassesEnchantments = 1ull << 10;
constexpr uint64_t kDamageTagBypassesInvulnerability = 1ull << 11;
constexpr uint64_t kDamageTagBypassesResistance = 1ull << 12;
constexpr uint64_t kDamageTagBypassesShield = 1ull << 13;
constexpr uint64_t kDamageTagBypassesWolfArmor = 1ull << 14;
constexpr uint64_t kDamageTagCanBreakArmorStand = 1ull << 15;
constexpr uint64_t kDamageTagDamagesHelmet = 1ull << 16;
constexpr uint64_t kDamageTagIgnitesArmorStands = 1ull << 17;
constexpr uint64_t kDamageTagIsDrowning = 1ull << 18;
constexpr uint64_t kDamageTagIsExplosion = 1ull << 19;
constexpr uint64_t kDamageTagIsFall = 1ull << 20;
constexpr uint64_t kDamageTagIsFire = 1ull << 21;
constexpr uint64_t kDamageTagIsFreezing = 1ull << 22;
constexpr uint64_t kDamageTagIsLightning = 1ull << 23;
constexpr uint64_t kDamageTagIsPlayerAttack = 1ull << 24;
constexpr uint64_t kDamageTagIsProjectile = 1ull << 25;
constexpr uint64_t kDamageTagMaceSmash = 1ull << 26;
constexpr uint64_t kDamageTagNoAnger = 1ull << 27;
constexpr uint64_t kDamageTagNoImpact = 1ull << 28;
constexpr uint64_t kDamageTagNoKnockback = 1ull << 29;
constexpr uint64_t kDamageTagNoWolfRetaliation = 1ull << 30;
constexpr uint64_t kDamageTagPanicCauses = 1ull << 31;
constexpr uint64_t kDamageTagPanicEnvironmentalCauses = 1ull << 32;
constexpr uint64_t kDamageTagSulfurCubeWithBlockImmuneTo = 1ull << 33;
constexpr uint64_t kDamageTagWitchResistantTo = 1ull << 34;
constexpr uint64_t kDamageTagWitherImmuneTo = 1ull << 35;

struct GeneratedDamageType {
  const char* name;           // minecraft:fall
  const char* message_id;     // fall (death.attack.<message_id>)
  uint8_t scaling;            // kDamageScaling*
  uint8_t death_message_type; // kDeathMessage*
  const char* effects;        // hurt|drowning|burning|poking|freezing
  float exhaustion;           // Player.causeFoodExhaustion per hit
  uint64_t tags;              // kDamageTag* bits
};

// clang-format off
inline constexpr GeneratedDamageType kDamageTypes[] = {
    {"minecraft:arrow", "arrow", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000282000002ull},
    {"minecraft:bad_respawn_point", "badRespawnPoint", kDamageScalingAlways, kDeathMessageIntentionalGameDesign, "hurt", 0.1f, 0x0000000220080011ull},
    {"minecraft:cactus", "cactus", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x00000003a0002000ull},
    {"minecraft:campfire", "inFire", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x0000000020222020ull},
    {"minecraft:cramming", "cramming", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020006080ull},
    {"minecraft:dragon_breath", "dragonBreath", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x00000000a0002080ull},
    {"minecraft:drown", "drown", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "drowning", 0.0f, 0x0000000830046080ull},
    {"minecraft:dry_out", "dryout", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000220006000ull},
    {"minecraft:ender_pearl", "fall", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageFallVariants, "hurt", 0.0f, 0x0000000020102080ull},
    {"minecraft:explosion", "explosion", kDamageScalingAlways, kDeathMessageDefault, "hurt", 0.1f, 0x00000002a0080011ull},
    {"minecraft:fall", "fall", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageFallVariants, "hurt", 0.0f, 0x0000000220102080ull},
    {"minecraft:falling_anvil", "anvil", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000200012000ull},
    {"minecraft:falling_block", "fallingBlock", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000200010000ull},
    {"minecraft:falling_stalactite", "fallingStalactite", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000200012000ull},
    {"minecraft:fireball", "fireball", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x0000000082200002ull},
    {"minecraft:fireworks", "fireworks", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000280080011ull},
    {"minecraft:fly_into_wall", "flyIntoWall", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020002080ull},
    {"minecraft:freeze", "freeze", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "freezing", 0.0f, 0x00000003a0406080ull},
    {"minecraft:generic", "generic", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020002080ull},
    {"minecraft:generic_kill", "genericKill", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020007880ull},
    {"minecraft:hot_floor", "hotFloor", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x00000003a0202020ull},
    {"minecraft:in_fire", "inFire", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x00000001a0222000ull},
    {"minecraft:in_wall", "inWall", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020006080ull},
    {"minecraft:indirect_magic", "indirectMagic", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000480006080ull},
    {"minecraft:lava", "lava", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x00000001a0202000ull},
    {"minecraft:lightning_bolt", "lightningBolt", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x00000001a0802000ull},
    {"minecraft:mace_smash", "mace_smash", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000285008000ull},
    {"minecraft:magic", "magic", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x00000004a0006098ull},
    {"minecraft:mob_attack", "mob", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000280000000ull},
    {"minecraft:mob_attack_no_aggro", "mob", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000208000000ull},
    {"minecraft:mob_projectile", "mob", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000282000000ull},
    {"minecraft:on_fire", "onFire", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.0f, 0x00000001a02020c0ull},
    {"minecraft:out_of_world", "outOfWorld", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020007884ull},
    {"minecraft:outside_border", "outsideBorder", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020006080ull},
    {"minecraft:player_attack", "player", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000281008000ull},
    {"minecraft:player_explosion", "explosion.player", kDamageScalingAlways, kDeathMessageDefault, "hurt", 0.1f, 0x00000002a0088011ull},
    {"minecraft:sonic_boom", "sonic_boom", kDamageScalingAlways, kDeathMessageDefault, "hurt", 0.0f, 0x0000000480002480ull},
    {"minecraft:spear", "spear", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x00000002a1008000ull},
    {"minecraft:spit", "mob", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000200000000ull},
    {"minecraft:stalagmite", "stalagmite", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000220102080ull},
    {"minecraft:starve", "starve", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x0000000020006280ull},
    {"minecraft:sting", "sting", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000280000000ull},
    {"minecraft:sulfur_cube_hot", "sulfurCubeHot", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x00000003e0202020ull},
    {"minecraft:sweet_berry_bush", "sweetBerryBush", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "poking", 0.1f, 0x0000000220002000ull},
    {"minecraft:thorns", "thorns", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "thorns", 0.1f, 0x0000000400004010ull},
    {"minecraft:thrown", "thrown", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000282000000ull},
    {"minecraft:trident", "trident", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000282000002ull},
    {"minecraft:unattributed_fireball", "onFire", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "burning", 0.1f, 0x0000000082200000ull},
    {"minecraft:wind_charge", "mob", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000282000002ull},
    {"minecraft:wither", "wither", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.0f, 0x00000000a0006080ull},
    {"minecraft:wither_skull", "witherSkull", kDamageScalingWhenCausedByLivingNonPlayer, kDeathMessageDefault, "hurt", 0.1f, 0x0000000082000002ull},
};
inline constexpr int kDamageTypeCount = 51;
// clang-format on

} // namespace mc::world
