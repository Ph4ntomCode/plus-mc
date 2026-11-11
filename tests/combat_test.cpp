// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the melee half of combat (Phase 4 milestone G): Player.attack's
// charge curve, its critical and sweep gates, Player.doSweepAttack's flat
// constant, and the enchantment hooks the Phase-3 table left open —
// EnchantmentHelper.modifyDamage/modifyKnockback and Sweeping Edge's attribute,
// each materialised from the generated table against a hand-built weapon.
//
// The melee hooks are the part worth pinning here rather than only on the wire:
// what a Sharpness V sword adds (and what a Smite V sword adds *only* to a
// zombie), which entity a requirement's tag is tested against, and that the
// sweep formula is `1 + ratio * base` instead of `ratio * base`. The session's
// end of it — the victim list, the packets — is tests/mobs_test.py's.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "world/combat.h"
#include "world/damage.h"
#include "world/enchantment.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/survival.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                             \
    const double va = (a), vb = (b);                                                               \
    if (std::fabs(va - vb) > (eps)) {                                                              \
      std::fprintf(stderr, "FAIL %s:%d: %s == %f, want %f\n", __FILE__, __LINE__, #a, va, vb);     \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A stack of one named item with one enchantment at `level`, which is what
// `/enchant` produces.
ItemStack enchanted(const char* name, const char* enchantment, int level) {
  ItemStack stack = ItemStack::of(name, 1);
  const GeneratedEnchantment* def = enchantment_by_name(enchantment);
  if (def)
    stack.set_enchantment(enchantment_id_of(*def), level);
  return stack;
}

// The melee hooks of one weapon against one victim, with a player swinging it —
// which is the `direct_attacker` a melee swing always has.
MeleeEnchantments melee(const ItemStack& weapon, const char* victim_type) {
  return item_melee_enchantments(weapon, victim_type, "minecraft:player");
}

// One swing of a fully charged diamond sword (7.0 damage, 1.6 attack speed, 13
// ticks of charge is a full one), on the ground, walking.
MeleeSwing sword_swing() {
  MeleeSwing swing;
  swing.base_damage = 7.0f;
  swing.attack_speed = 1.6f;
  swing.charge_ticks = 13;
  swing.on_ground = true;
  swing.sword = true;
  return swing;
}

} // namespace

int main() {
  // --- the charge curve -----------------------------------------------------
  // An empty hand needs five ticks for a full charge; the half-tick offset is
  // the same one Player.baseDamageScaleFactor uses.
  CHECK_NEAR(attack_strength(0, 4.0f), 0.1f, 1e-6);
  CHECK_NEAR(attack_strength(100, 4.0f), 1.0f, 1e-6);

  // --- the swing ------------------------------------------------------------
  // A falling, fully charged, non-sprinting swing is a critical: 7.0 * 1.5.
  MeleeSwing critical_swing = sword_swing();
  critical_swing.falling = true;
  critical_swing.on_ground = false;
  const CombatAttack critical = player_attack(critical_swing);
  CHECK(critical.fully_charged && critical.critical);
  CHECK_NEAR(critical.damage, 10.5f, 1e-5);
  CHECK_NEAR(critical.base_damage, 10.5f, 1e-5);
  // Player.isSweepAttack refuses a critical swing outright.
  CHECK(!critical.sweeping);

  // ...and Player.canCriticalAttack's `!isMobilityRestricted()` takes the
  // critical away again: `isMobilityRestricted()` is `hasEffect(BLINDNESS)`, so
  // the same airborne charge is the plain 7.0. Being no longer a critical it
  // would be allowed to sweep, but it is still in the air, so it does not.
  MeleeSwing blinded_swing = sword_swing();
  blinded_swing.falling = true;
  blinded_swing.on_ground = false;
  blinded_swing.mobility_restricted = true;
  const CombatAttack blinded = player_attack(blinded_swing);
  CHECK(blinded.fully_charged && !blinded.critical);
  CHECK_NEAR(blinded.damage, 7.0f, 1e-5);
  CHECK(!blinded.sweeping);

  // A sprinting full-charge swing is the knockback attack instead, so it does
  // not sweep either, and it adds Player.attack's 0.5 to getKnockback.
  MeleeSwing sprint_swing = sword_swing();
  sprint_swing.sprinting = true;
  sprint_swing.enchants =
      melee(enchanted("minecraft:diamond_sword", "knockback", 2), "minecraft:zombie");
  const CombatAttack sprint = player_attack(sprint_swing);
  CHECK(!sprint.critical && !sprint.sweeping);
  // Knockback II is `knockback_add` 2.0, halved by getKnockback, plus the 0.5.
  CHECK_NEAR(sprint.knockback, 1.5, 1e-6);

  // The sweep gate: full charge, on the ground, not sprinting, and a sword.
  const CombatAttack plain = player_attack(sword_swing());
  CHECK(plain.sweeping);
  MeleeSwing no_sword = sword_swing();
  no_sword.sword = false;
  CHECK(!player_attack(no_sword).sweeping);
  MeleeSwing half_charged = sword_swing();
  half_charged.charge_ticks = 1;
  CHECK(!player_attack(half_charged).sweeping);

  // --- the mace's smash -----------------------------------------------------
  // MaceItem.canSmashAttack: more than 1.5 blocks fallen, and not an elytra
  // flight. The threshold is strict, and it is deliberately *not* the
  // critical's `fallDistance > 0.0` — a mace swung a block into a fall does
  // neither.
  CHECK(!can_smash_attack(0.0, false));
  CHECK(!can_smash_attack(1.5, false));
  CHECK(can_smash_attack(1.6, false));
  CHECK(!can_smash_attack(3.0, true)); // a fall under an elytra refuses it

  // MaceItem.getAttackDamageBonus' curve is three segments, continuous at both
  // joints (4 x 3 = 12 and 12 + 2 x 5 = 22): 4 a block to three, 2 a block to
  // eight, 1 a block after. Density's own add is the last term, multiplied by
  // the fall distance the way vanilla multiplies it.
  CHECK_NEAR(mace_smash_bonus(0.0, 0.0f), 0.0f, 1e-6);
  CHECK_NEAR(mace_smash_bonus(1.6, 0.0f), 6.4f, 1e-5);
  CHECK_NEAR(mace_smash_bonus(3.0, 0.0f), 12.0f, 1e-5);
  CHECK_NEAR(mace_smash_bonus(8.0, 0.0f), 22.0f, 1e-5);
  CHECK_NEAR(mace_smash_bonus(10.0, 0.0f), 24.0f, 1e-5);
  CHECK_NEAR(mace_smash_bonus(3.0, 2.5f), 19.5f, 1e-5);
  CHECK_NEAR(mace_smash_bonus(10.0, 1.0f), 34.0f, 1e-5);

  // The swing it lands on: a mace is 6.0 (the player's base 1.0 plus its 5.0),
  // the smash's add is flat — not scaled by the charge, because Player.attack
  // adds it to the already-scaled baseDamage — and the 1.5 critical lands on
  // the *sum*, which is what makes the airborne 1.6-block case 18.6 and not
  // the 15.4 that a crit-then-bonus order would give.
  MeleeSwing mace_swing;
  mace_swing.base_damage = 6.0f;
  mace_swing.attack_speed = 0.6f; // the mace's 4.0 + (-3.4): a 33-tick charge
  mace_swing.charge_ticks = 34;
  mace_swing.mace = true;
  const CombatAttack mace_standing = player_attack(mace_swing);
  CHECK(!mace_standing.smash && mace_standing.fully_charged);
  CHECK_NEAR(mace_standing.damage, 6.0f, 1e-5);
  mace_swing.falling = true;
  mace_swing.on_ground = false;
  mace_swing.fall_distance = 1.6;
  const CombatAttack smash = player_attack(mace_swing);
  CHECK(smash.smash && smash.critical);
  CHECK_NEAR(smash.base_damage, (6.0f + 6.4f) * 1.5f, 1e-5);
  CHECK_NEAR(smash.damage, 18.6f, 1e-5);
  // Blindness takes the critical away and leaves the smash: 6.0 + 6.4. The two
  // gates are separate questions, and only one of them reads the effect.
  MeleeSwing blinded_smash = mace_swing;
  blinded_smash.mobility_restricted = true;
  CHECK(player_attack(blinded_smash).smash && !player_attack(blinded_smash).critical);
  CHECK_NEAR(player_attack(blinded_smash).damage, 12.4f, 1e-5);
  // ...and Density's 2.5 a block rides the same term: 6.0 + 6.4 + 4.0, x1.5.
  MeleeSwing density_smash = mace_swing;
  density_smash.enchants.smash_damage = 2.5f;
  CHECK_NEAR(player_attack(density_smash).damage, (6.0f + 6.4f + 4.0f) * 1.5f, 1e-5);
  // The three inputs are separate: a mace below the threshold has no smash, and
  // a sword at the same fall distance has none either, however it is falling.
  MeleeSwing mace_below = mace_swing;
  mace_below.fall_distance = 1.4;
  CHECK(!player_attack(mace_below).smash);
  MeleeSwing sword_falling = sword_swing();
  sword_falling.falling = true;
  sword_falling.on_ground = false;
  sword_falling.fall_distance = 3.0;
  CHECK(!player_attack(sword_falling).smash);

  // MaceItem.getKnockbackPower: `(3.5 - distance) * 0.7`, doubled only for a
  // fall of *more* than five blocks, scaled by the nearby entity's resistance.
  CHECK_NEAR(smash_knockback_power(0.0, 3.0, 0.0), 2.45, 1e-6);
  CHECK_NEAR(smash_knockback_power(0.0, 5.0, 0.0), 2.45, 1e-6); // not *at* five
  CHECK_NEAR(smash_knockback_power(0.0, 6.0, 0.0), 4.9, 1e-6);
  CHECK_NEAR(smash_knockback_power(0.0, 6.0, 0.5), 2.45, 1e-6);
  CHECK_NEAR(smash_knockback_power(2.0, 6.0, 0.0), 2.1, 1e-6);
  CHECK_NEAR(smash_knockback_power(3.5, 6.0, 0.0), 0.0, 1e-6);

  // MaceItem.knockbackPredicate's answerable clauses. It is one function rather
  // than a condition in each sweep because the two populations have to answer it
  // the same way: four of the five clauses are the candidate's alone, and only
  // `isCreative() && abilities.flying` needs anything this server has to
  // *track* — the serverbound abilities packet.
  CHECK(smash_knockback_candidate(false, false, false, false));
  CHECK(!smash_knockback_candidate(true, false, false, false)); // the victim
  CHECK(!smash_knockback_candidate(false, true, false, false)); // the attacker
  CHECK(!smash_knockback_candidate(false, false, true, false)); // a spectator
  CHECK(!smash_knockback_candidate(false, false, false, true)); // flying, creative

  // MaceItem.knockback's push for one candidate: the unit direction times the
  // power, with the flat 0.7 vertical. One block away on x is
  // `(3.5 - 1) * 0.7` = 1.75 along x and the same 0.7 up whatever the distance.
  double push_x = 0.0, push_y = 0.0, push_z = 0.0;
  CHECK(smash_push(1.0, 0.0, 0.0, 3.0, 0.0, &push_x, &push_y, &push_z));
  CHECK_NEAR(push_x, 1.75, 1e-6);
  CHECK_NEAR(push_y, 0.7, 1e-6);
  CHECK_NEAR(push_z, 0.0, 1e-9);
  // A 3-4-0 candidate is five blocks out: past `kSmashKnockbackRadius`, so no
  // push at all — and nothing special-cases the radius, because the factor has
  // already reached zero by there. The boundary itself is the same answer.
  CHECK(!smash_push(3.0, 4.0, 0.0, 6.0, 0.0, &push_x, &push_y, &push_z));
  CHECK(!smash_push(kSmashKnockbackRadius, 0.0, 0.0, 3.0, 0.0, &push_x, &push_y, &push_z));
  // Vec3.normalize of the zero vector is the zero vector: a candidate sitting on
  // the victim is pushed straight up rather than in a direction that is noise.
  CHECK(smash_push(0.0, 0.0, 0.0, 3.0, 0.0, &push_x, &push_y, &push_z));
  CHECK_NEAR(push_x, 0.0, 1e-9);
  CHECK_NEAR(push_y, 0.7, 1e-6);
  CHECK_NEAR(push_z, 0.0, 1e-9);
  // The heavy doubling is the fall-distance term's, so it rides the same call:
  // six blocks of fall doubles the horizontal and leaves the 0.7 vertical alone.
  CHECK(smash_push(1.0, 0.0, 0.0, 6.0, 0.0, &push_x, &push_y, &push_z));
  CHECK_NEAR(push_x, 3.5, 1e-6);
  CHECK_NEAR(push_y, 0.7, 1e-6);
  CHECK(smash_push(1.0, 0.0, 0.0, 6.0, 0.5, &push_x, &push_y, &push_z));
  CHECK_NEAR(push_x, 1.75, 1e-6); // ...and resistance halves it again

  // --- the enchantment hooks ------------------------------------------------
  // Sharpness V is the linear 1.0 + 0.5 * 4, against anything at all.
  const ItemStack sharp5 = enchanted("minecraft:diamond_sword", "sharpness", 5);
  const MeleeEnchantments sharp = melee(sharp5, "minecraft:pig");
  CHECK_NEAR(sharp.damage, 3.0f, 1e-6);
  CHECK_NEAR(sharp.knockback, 0.0f, 1e-6);
  CHECK_NEAR(sharp.sweep_ratio, 0.0f, 1e-6);
  CHECK_NEAR(sharp.smash_damage, 0.0f, 1e-6);

  // Density is the one enchantment whose component is a *rate* rather than a
  // bonus: five levels of smash_damage_per_fallen_block are the linear
  // 0.5 + 0.5 * 4 = 2.5 a block fallen, read off the mace the same way
  // Sharpness is read off a sword — and it is the only value that enchantment
  // carries, which is what its damage and knockback zeros say.
  const ItemStack density5 = enchanted("minecraft:mace", "density", 5);
  CHECK_NEAR(melee(density5, "minecraft:zombie").smash_damage, 2.5f, 1e-6);
  CHECK_NEAR(melee(density5, "minecraft:zombie").damage, 0.0f, 1e-6);
  CHECK_NEAR(melee(density5, "minecraft:zombie").knockback, 0.0f, 1e-6);

  // Smite V is 2.5 + 2.5 * 4 = 12.5, and only against the entity types in
  // #minecraft:sensitive_to_smite: a zombie yes, a pig no.
  const ItemStack smite5 = enchanted("minecraft:diamond_sword", "smite", 5);
  CHECK_NEAR(melee(smite5, "minecraft:zombie").damage, 12.5f, 1e-6);
  CHECK_NEAR(melee(smite5, "minecraft:husk").damage, 12.5f, 1e-6);
  CHECK_NEAR(melee(smite5, "minecraft:pig").damage, 0.0f, 1e-6);
  // ...and Bane of Arthropods' and Impaling's own tags, which the same lookup
  // resolves: a spider is arthropod-sensitive and not smite-sensitive, and a
  // turtle is impaling-sensitive and neither of the others.
  const ItemStack bane5 = enchanted("minecraft:diamond_sword", "bane_of_arthropods", 5);
  CHECK_NEAR(melee(bane5, "minecraft:spider").damage, 12.5f, 1e-6);
  CHECK_NEAR(melee(bane5, "minecraft:zombie").damage, 0.0f, 1e-6);
  const ItemStack impaling5 = enchanted("minecraft:diamond_sword", "impaling", 5);
  CHECK_NEAR(melee(impaling5, "minecraft:turtle").damage, 12.5f, 1e-6);
  CHECK_NEAR(melee(impaling5, "minecraft:spider").damage, 0.0f, 1e-6);

  // Knockback II and Punch II carry the same value, but Punch's requirement is
  // on the *attacker*: a melee swing by a player adds nothing, while the same
  // swing credited to an arrow adds the whole 2.0.
  const ItemStack knock2 = enchanted("minecraft:diamond_sword", "knockback", 2);
  CHECK_NEAR(melee(knock2, "minecraft:pig").knockback, 2.0f, 1e-6);
  const ItemStack punch2 = enchanted("minecraft:bow", "punch", 2);
  CHECK_NEAR(melee(punch2, "minecraft:pig").knockback, 0.0f, 1e-6);
  CHECK_NEAR(item_melee_enchantments(punch2, "minecraft:pig", "minecraft:arrow").knockback, 2.0f,
             1e-6);
  const ItemStack power5 = enchanted("minecraft:bow", "power", 5);
  CHECK_NEAR(melee(power5, "minecraft:pig").damage, 0.0f, 1e-6);
  CHECK_NEAR(item_melee_enchantments(power5, "minecraft:pig", "minecraft:arrow").damage, 3.0f,
             1e-6);

  // Sweeping Edge III is the fraction 3/4 — numerator 1 + 2, denominator 2 + 2 —
  // while an enchantment whose minecraft:attributes is something else (Depth
  // Strider's movement efficiency) contributes no ratio at all.
  const ItemStack sweep3 = enchanted("minecraft:diamond_sword", "sweeping_edge", 3);
  CHECK_NEAR(melee(sweep3, "minecraft:pig").sweep_ratio, 0.75f, 1e-6);
  CHECK_NEAR(
      melee(enchanted("minecraft:diamond_boots", "depth_strider", 3), "minecraft:pig").sweep_ratio,
      0.0f, 1e-6);
  // An enchantment with neither value component (Fortune's is minecraft:block_
  // drops) reads as nothing rather than as a zero it computed.
  const MeleeEnchantments fortune =
      melee(enchanted("minecraft:diamond_pickaxe", "fortune", 3), "minecraft:pig");
  CHECK_NEAR(fortune.damage, 0.0f, 1e-6);
  CHECK_NEAR(fortune.knockback, 0.0f, 1e-6);
  CHECK_NEAR(fortune.sweep_ratio, 0.0f, 1e-6);
  CHECK_NEAR(fortune.smash_damage, 0.0f, 1e-6);

  // The whole swing, with the hooks folded in: a fully charged Sharpness V
  // diamond sword lands 7.0 + 3.0, and the sweep it starts still scales the
  // weapon's own 7.0 (`baseDamage` at the doSweepAttack call is post-crit and
  // pre-magicBoost).
  MeleeSwing sharp_swing = sword_swing();
  sharp_swing.enchants = sharp;
  const CombatAttack enchanted_hit = player_attack(sharp_swing);
  CHECK(enchanted_hit.sweeping);
  CHECK_NEAR(enchanted_hit.base_damage, 7.0f, 1e-5);
  CHECK_NEAR(enchanted_hit.damage, 10.0f, 1e-5);
  // Half a charge scales the enchantment's share too: scale * 3.0, on top of the
  // 0.2 + scale^2 * 0.8 curve the weapon's own damage takes.
  MeleeSwing half_sharp = sharp_swing;
  half_sharp.charge_ticks = 6;
  const CombatAttack half_hit = player_attack(half_sharp);
  CHECK_NEAR(half_hit.damage,
             7.0 * (0.2 + half_hit.strength * half_hit.strength * 0.8) + half_hit.strength * 3.0,
             1e-5);
  CHECK(half_hit.damage > 7.0 * (0.2 + half_hit.strength * half_hit.strength * 0.8));

  // --- the sweep's own arithmetic -------------------------------------------
  // The flat 1.0 is the whole of an unenchanted, un-Sweeping-Edged sweep: the
  // ratio is 0, so `1 + 0 * base` is 1.0 and never `ratio * base` = 0.
  CHECK_NEAR(sweeping_damage(7.0f, 0.0f, 0.0f, 1.0f), 1.0f, 1e-6);
  // Sweeping Edge III widens it to 1 + 0.75 * 7 = 6.25 ...
  CHECK_NEAR(sweeping_damage(7.0f, 0.75f, 0.0f, 1.0f), 6.25f, 1e-5);
  // ... and a swept zombie gets the weapon's Smite too, because the sweep
  // resolves the hooks against each victim it reaches.
  CHECK_NEAR(sweeping_damage(7.0f, 0.75f, 12.5f, 1.0f), 18.75f, 1e-5);
  // The strength scale is the last factor, exactly as doSweepAttack multiplies.
  CHECK_NEAR(sweeping_damage(7.0f, 0.75f, 0.0f, 0.5f), 3.125f, 1e-5);

  // --- knockback ------------------------------------------------------------
  CHECK_NEAR(attack_knockback(0.5, 2.0f, 0.25), 1.125, 1e-6);
  // No modelled mob sets KNOCKBACK_RESISTANCE, but the factor is the victim's:
  // full resistance refuses the push outright.
  CHECK_NEAR(attack_knockback(0.5, 2.0f, 1.0), 0.0, 1e-6);

  // --- the victim's own post-attack entries (Thorns) -------------------------
  //
  // The other half of `minecraft:post_attack`: the entry the *victim's*
  // equipment lands on whoever hit it. Thorns' chance is the random_chance
  // requirement, 0.15 a level (so III is 0.45), the damage is the 1..5 the
  // entry rolls between, the damage type names the registry entry the hurt is
  // credited to the wearer with, and the durability half is 2.
  {
    EquipmentEntry worn[1];
    worn[0].slot = kEnchantSlotChest;
    worn[0].stack = enchanted("minecraft:diamond_chestplate", "thorns", 3);
    ThornsEffect effects[kMaxThorns];
    const int count = victim_post_attack_effects(worn, 1, effects, kMaxThorns);
    CHECK(count == 1);
    CHECK(std::strcmp(effects[0].damage_type, "minecraft:thorns") == 0);
    CHECK_NEAR(effects[0].chance, 0.45f, 1e-6);
    CHECK_NEAR(effects[0].min_damage, 1.0f, 1e-6);
    CHECK_NEAR(effects[0].max_damage, 5.0f, 1e-6);
    CHECK_NEAR(effects[0].item_damage, 2.0f, 0.0);
    CHECK(effects[0].slot == kEnchantSlotChest);
  }
  // The level scales the chance, not the damage: I is 0.15, II is 0.30, and
  // both still roll between the same 1 and 5.
  {
    EquipmentEntry worn[2];
    worn[0].slot = kEnchantSlotLegs;
    worn[0].stack = enchanted("minecraft:diamond_leggings", "thorns", 1);
    worn[1].slot = kEnchantSlotHead;
    worn[1].stack = enchanted("minecraft:diamond_helmet", "thorns", 2);
    ThornsEffect effects[kMaxThorns];
    const int count = victim_post_attack_effects(worn, 2, effects, kMaxThorns);
    CHECK(count == 2);
    CHECK_NEAR(effects[0].chance, 0.15f, 1e-6);
    CHECK_NEAR(effects[1].chance, 0.30f, 1e-6);
    CHECK_NEAR(effects[1].min_damage, 1.0f, 1e-6);
    CHECK_NEAR(effects[1].max_damage, 5.0f, 1e-6);
    // Vanilla walks the equipment in slot order and so does the caller's list:
    // each entry keeps the slot it came from.
    CHECK(effects[0].slot == kEnchantSlotLegs);
    CHECK(effects[1].slot == kEnchantSlotHead);
  }
  // A piece that carries no Thorns, and an empty slot, contribute nothing — and
  // the clamp is honoured rather than overrun when a whole set is worn.
  {
    EquipmentEntry worn[4];
    worn[0].slot = kEnchantSlotFeet;
    worn[0].stack = enchanted("minecraft:diamond_boots", "thorns", 3);
    worn[1].slot = kEnchantSlotLegs;
    worn[1].stack = enchanted("minecraft:diamond_leggings", "thorns", 3);
    worn[2].slot = kEnchantSlotChest;
    worn[2].stack = enchanted("minecraft:diamond_chestplate", "protection", 4);
    worn[3].slot = kEnchantSlotHead; // empty
    ThornsEffect effects[kMaxThorns];
    CHECK(victim_post_attack_effects(worn, 4, effects, kMaxThorns) == 2);
    CHECK(victim_post_attack_effects(worn, 4, effects, 1) == 1);
    CHECK(victim_post_attack_effects(nullptr, 0, effects, kMaxThorns) == 0);
  }
  // The weapon-side resolver does not pick Thorns up: the entry is the victim's
  // (`enchanted: victim`), so a melee swing with a Thorns piece in hand adds
  // nothing to the swing itself.
  CHECK(melee(enchanted("minecraft:diamond_chestplate", "thorns", 3), "minecraft:zombie")
            .post_attack_count == 0);

  // --- shields --------------------------------------------------------------
  CHECK(shield_blocks_damage(true, false));
  CHECK(!shield_blocks_damage(true, true));
  CHECK(!shield_blocks_damage(false, false));

  // Shield blocking stops health loss but still accepts the hit as a handled
  // damage event; bypass_shield damage remains effective.
  SurvivalState state;
  SurvivalContext shielded;
  shielded.shield_blocking = true;
  const auto* attack = damage_type_by_name("player_attack");
  const DamageResult blocked = apply_damage(state, source_of(*attack), 10.0f, shielded);
  CHECK(blocked.accepted && blocked.applied == 0.0f && state.health == 20.0f);
  const auto* bypass = damage_type_by_name("out_of_world");
  const DamageResult pierced = apply_damage(state, source_of(*bypass), 2.0f, shielded);
  CHECK(pierced.accepted && state.health < 20.0f);

  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("combat: all checks passed\n");
  return 0;
}
