// SPDX-License-Identifier: GPL-3.0-or-later
//
// Enchantments: the minecraft:enchantment registry as the engine reads it, and
// the two parts of the enchantment machinery a hit needs —
// EnchantmentHelper.getDamageProtection (Phase 3 milestone G) and the melee
// value effects, EnchantmentHelper.modifyDamage/modifyKnockback (Phase 4
// milestone G).
//
// The table itself is generated (world/enchantment_table.h) from
// data/minecraft/enchantment plus the captured wire order, because
// minecraft:enchantment is a *datapack* registry: the id that a
// `minecraft:enchantments` item component carries is the index in the order the
// client was told during configuration, not any built-in registration order.
//
// What is modelled, and what is deliberately not: of the 31 data-driven
// enchantment effect components, six are applied. `minecraft:damage_protection`
// is what makes Protection, Blast Protection, Fire Protection, Projectile
// Protection and Feather Falling turn damage aside; `minecraft:damage` and
// `minecraft:knockback` are the melee hooks — Sharpness' unconditional add,
// Smite's and Bane of Arthropods' against the victim's own entity type,
// Impaling's likewise, Knockback's and Punch's push;
// `minecraft:smash_damage_per_fallen_block` is Density's damage per fallen
// block, the one hook that is a *rate* rather than a flat bonus (the mace's
// smash multiplies it by the attacker's fall distance — world/combat.h's
// `mace_smash_bonus`); and `minecraft:attributes` is read for Sweeping Edge's
// `sweeping_damage_ratio` alone; and `minecraft:post_attack` is read for the
// entries that land on the *victim* — Fire Aspect's four-seconds-a-level ignite
// and Bane of Arthropods' Slowness IV — as the resolved `PostAttackEffect` list
// below (`item_melee_enchantments` computes it, the session rolls and applies
// it). The generated table records which components every enchantment carries
// (the `effects` bitmask), so an enchantment whose only effect is, say,
// `minecraft:projectile_piercing` (Piercing), or a post-attack entry aimed at
// the *attacker* (Thorns' damage, Wind Burst's gust), is *visible* as
// unimplemented rather than silently inert — see docs/PHASE3.md for the list.
//
// Two of those five reach the entity-type tags vanilla gates them on. The
// generator expands `#minecraft:sensitive_to_smite` and friends into
// world/entity_type_tags.h (nested tag references resolved), so the requirement
// is evaluated against a *name* and stays correct for a type this engine has no
// mob row for — which is the reason Power's `minecraft:arrows` gate is
// implementable here at all.
//
// An item's enchantments live on the ItemStack (see world/item.h), and the
// `minecraft:enchantments` component is written and read on the wire, so a real
// client draws the glint and lists them in the tooltip.
#pragma once

#include <cstdint>
#include <string_view>

#include "world/damage_table.h"
#include "world/enchantment_table.h"
#include "world/item.h"

namespace mc::world {

// --- the registry -----------------------------------------------------------

// nullptr for an unknown or malformed name ("protection" is normalized first,
// and so is the full "minecraft:protection").
const GeneratedEnchantment* enchantment_by_name(std::string_view name);
// nullptr when the id is out of range.
const GeneratedEnchantment* enchantment_by_id(int32_t id);
// Registry index (== the wire id), or -1.
int32_t enchantment_id_of(const GeneratedEnchantment& enchantment);

// --- applicability ----------------------------------------------------------

// Enchantment.canEnchant(ItemStack): `supported_items.contains(item)`. This is
// the check `/enchant` reports as `commands.enchant.failed.incompatible`.
bool enchantment_supports_item(const GeneratedEnchantment& enchantment, int32_t item_id);

// Enchantment.matchingSlot for one slot bit (kEnchantSlot*). The mask is a union
// of single-slot bits, so this is the whole of EquipmentSlotGroup.test.
bool enchantment_applies_to_slot(const GeneratedEnchantment& enchantment, uint8_t slot);

// EnchantmentHelper.isEnchantmentCompatible: no existing enchantment may be
// incompatible with the candidate, where Enchantment.areCompatible is false for
// two *equal* holders as well as for an exclusive set. The consequence is
// vanilla's: `/enchant` on an item that already carries the same enchantment is
// refused as incompatible, even at a different level.
bool enchantment_compatible_with(const ItemStack& stack, const GeneratedEnchantment& candidate);

// --- damage protection ------------------------------------------------------

// One equipment slot as EnchantmentHelper.runIterationOnEquipment walks them:
// every slot an enchantment can sit in, with the kEnchantSlot* bit that
// Enchantment.matchingSlot tests. The stack is held by value rather than by
// pointer so an equipment list can outlive the inventory it was read from —
// `runIterationOnEquipment` copies nothing, but a borrowed pointer into a
// mutated inventory is the kind of thing that stops being true later. An empty
// stack is skipped, as vanilla skips one.
struct EquipmentEntry {
  uint8_t slot = 0;
  ItemStack stack;
};

// The summed minecraft:damage_protection of one stack in one slot against
// `type`: 0.0 when the stack carries nothing that applies. Exposed separately
// so a test can pin a single piece.
float item_damage_protection(const ItemStack& stack, uint8_t slot, const GeneratedDamageType& type);

// EnchantmentHelper.getDamageProtection: the total the equipment contributes,
// which LivingEntity.getDamageAfterMagicAbsorb hands to
// CombatRules.getDamageAfterMagicAbsorb.
float damage_protection(const EquipmentEntry* equipment, int count,
                        const GeneratedDamageType& type);

// --- the melee hooks --------------------------------------------------------

// The most post-attack entries one weapon can carry. The vanilla data has two
// (Fire Aspect's ignite and Bane of Arthropods' slowness), and a weapon can hold
// both, so the array is never full by two.
constexpr int kMaxPostAttack = 4;

// EnchantmentHelper.doPostAttackEffects' applied half: one `minecraft:post_attack`
// entry a weapon carries against a victim, with its level-based values already
// resolved for the level the attacker's item has them at.
//
// The durations are floored to ticks the way vanilla floors them
// (`Mth.floor(seconds * 20.0F)`) *before* the roll, and the pair is what vanilla
// rolls between: Fire Aspect has one duration and no roll (`min == max`), while
// Bane of Arthropods' Slowness IV has both a duration range and an amplifier
// range. The roll itself is the session's, because vanilla draws it from the
// *victim's* random source (`Mth.nextInt(entity.getRandom(), min, max)`) and a
// mob's random source belongs to whatever simulates it.
struct PostAttackEffect {
  uint8_t kind = 0;             // kPostAttackIgnite / kPostAttackApplyMobEffect
  const char* effect = nullptr; // apply_mob_effect's to_apply; nullptr for ignite
  int min_ticks = 0;            // the data's min_duration, floored
  int max_ticks = 0;            // ...and max_duration; equal for ignite
  int min_amplifier = 0;        // ...and the amplifier pair, in levels
  int max_amplifier = 0;
};

// The value effects a *weapon* contributes against one victim, as the four
// deltas the swing consumes. Nothing here depends on a damage type: a melee
// swing's source is `minecraft:player_attack` whatever the weapon, and
// `minecraft:mace_smash` while the mace's smash gate holds, and no one of the
// four reads it.
struct MeleeEnchantments {
  float damage = 0.0f;      // EnchantmentHelper.modifyDamage's delta
  float knockback = 0.0f;   // EnchantmentHelper.modifyKnockback's delta
  float sweep_ratio = 0.0f; // Attributes.SWEEPING_DAMAGE_RATIO (Sweeping Edge)
  // EnchantmentHelper.modifyFallBasedDamage's delta — Density's add per fallen
  // block, which only the mace's smash has a fall distance for.
  float smash_damage = 0.0f;
  // EnchantmentHelper.doPostAttackEffects: the entries that land on the victim,
  // in the order the weapon's enchantments list them (Fire Aspect, Bane of
  // Arthropods). The session applies each to the entity the hit landed on, for
  // both the struck victim and every swept one.
  PostAttackEffect post_attack[kMaxPostAttack];
  int post_attack_count = 0;
};

// The most attacker-facing post-attack entries one set of equipment can carry.
// The vanilla data has exactly one (Thorns), and each enchanted piece can carry
// it, so four armour slots can contribute four.
constexpr int kMaxThorns = 4;

// EnchantmentHelper.doPostAttackEffects' *victim-side* half: one
// `minecraft:post_attack` entry the victim's own equipment lands on whoever hurt
// it, resolved for the level the enchanted piece carries. Thorns is the only
// entry in the 26.2 data that fits.
//
// Vanilla walks the same component twice — the victim's equipment with
// `EnchantmentTarget.VICTIM` and the attacker's weapon with
// `EnchantmentTarget.ATTACKER` — and this is the first walk's `affected: attacker`
// half. `Enchantment.doPostAttack` resolves `attacker` to
// `damageSource.getEntity()` and `DamageEntity` hurts *that* entity with a source
// whose causing entity is the enchanted piece's owner, which is why a thorns
// kill is credited to the player rather than to the mob.
//
// The chance is the entry's `minecraft:random_chance` requirement (`0.15 x
// level` for Thorns), rolled before anything lands, and the damage is
// `Mth.randomBetween(min, max)`: both draws need a random source, and vanilla
// takes them from the *hurt* entity's own (`entity.getRandom()`), so the caller
// owns them here the way it owns the `PostAttackEffect` rolls above.
//
// `item_damage` is `ChangeItemDamage`'s amount: the durability the enchanted
// piece itself loses when the entry fires. It is resolved here and applied by
// the session (which owns the inventory), off the stack in `slot` — the slot the
// entry came from, which is what `EnchantedItemInUse` carries in vanilla. A
// piece that is not damageable, or whose player has infinite materials, wears
// nothing (`ItemStack.hurtAndBreak`'s own two gates).
struct ThornsEffect {
  uint8_t slot = 0;                  // the equipment slot the entry came from
  const char* damage_type = nullptr; // the damage type the attacker takes
  float chance = 0.0f;               // random_chance, resolved for the level
  float min_damage = 0.0f;           // Mth.randomBetween's bounds, likewise
  float max_damage = 0.0f;
  int item_damage = 0; // change_item_damage, in durability
};

// EnchantmentHelper.runIterationOnEquipment for one victim, filtered to the
// attacker-facing entries: every `minecraft:post_attack` entry the equipment's
// own enchantments land on the entity that hurt this one, in the equipment
// list's own order (an empty stack contributes nothing). `out` holds at most
// `max_out` and the return value is how many were written.
int victim_post_attack_effects(const EquipmentEntry* equipment, int count, ThornsEffect* out,
                               int max_out);

// EnchantmentHelper.modifyDamage + modifyKnockback + modifyFallBasedDamage +
// AttributeInstance for one weapon stack, against one victim.
//
// `victim_type` is the `minecraft:entity_type` name the requirements' `entity:
// this` tag is tested against (Smite's zombie) and `attacker_type` the one a
// `direct_attacker` requirement is (Power's arrow, which a player's melee swing
// never is). Both are names rather than ids for the reason the tag table holds
// names: vanilla tests the tag, not a registry entry this engine happens to have.
MeleeEnchantments item_melee_enchantments(const ItemStack& stack, std::string_view victim_type,
                                          std::string_view attacker_type);

} // namespace mc::world
