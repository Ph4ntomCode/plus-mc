// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the survival layer (Phase 3 milestone F):
//   * the generated damage-type table, its tags and the death-message keys,
//   * the armour lookup and CombatRules' two absorb formulas,
//   * armour durability: `LivingEntity.doHurtEquipment`'s per-piece amount and
//     the damage it is charged on (`DamageResult.landed`),
//   * Player.hurtServer + LivingEntity.hurt + actuallyHurt: the creative gate,
//     the invulnerability window, armour, absorption and exhaustion per hit,
//   * FoodData.tick: saturation regeneration, the slow bar, starvation,
//   * LivingEntity's air supply and drowning,
//   * Entity.checkFallDamage and LivingEntity.calculateFallDamage,
//   * FoodData.eat / Player.canEat and the item table's food columns,
//   * the packets: set_health, damage_event, entity_event, update_attributes
//     (with armour modifiers), respawn, player_combat_kill, set_equipment.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "server/entity_packets.h"
#include "world/attribute_table.h"
#include "world/damage.h"
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

#define CHECK_BYTES(vec, offset, literal)                                                          \
  do {                                                                                             \
    const std::string want = (literal);                                                            \
    const std::string got((vec).begin() + (offset), (vec).begin() + (offset) + want.size());       \
    if (got != want) {                                                                             \
      std::fprintf(stderr, "FAIL %s:%d: bytes@%d == \"%s\", want \"%s\"\n", __FILE__, __LINE__,    \
                   static_cast<int>(offset), got.c_str(), want.c_str());                           \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t item(const char* name) {
  auto id = ItemRegistry::instance().id_of(name);
  return id ? *id : -1;
}

ItemStack stack(const char* name, int count = 1) {
  return ItemStack::of_id(item(name), count);
}

int32_t index_of(const char* name) {
  for (int i = 0; i < kDamageTypeCount; ++i)
    if (name == std::string(kDamageTypes[i].name))
      return i;
  return -1;
}

// Armour is four stacks in inventory order: feet, legs, chest, head.
void armour_set(ItemStack* slots, const char* feet, const char* legs, const char* chest,
                const char* head) {
  const char* names[4] = {feet, legs, chest, head};
  for (int i = 0; i < 4; ++i)
    slots[i] = names[i] ? stack(names[i]) : ItemStack{};
}

SurvivalContext context_with(const ItemStack* armour) {
  SurvivalContext ctx;
  if (armour)
    ctx.armor = armor_values(armour, 4);
  return ctx;
}

// apply_damage with a bare registry type, which is how every caller outside
// /damage builds a source.
DamageResult hit(SurvivalState& state, const GeneratedDamageType& type, float amount,
                 const SurvivalContext& ctx) {
  return apply_damage(state, source_of(type), amount, ctx);
}

void test_damage_table() {
  // The generated table *is* the wire order (minecraft:damage_type is a
  // synchronized registry), so both ends pin it to vanilla's list.
  CHECK(kDamageTypeCount == 51);
  CHECK(std::string(kDamageTypes[0].name) == "minecraft:arrow");
  CHECK(std::string(kDamageTypes[kDamageTypeCount - 1].name) == "minecraft:wither_skull");

  const GeneratedDamageType* fall = damage_type_by_name("fall");
  CHECK(fall != nullptr);
  CHECK(fall == damage_type_by_name("minecraft:fall"));
  CHECK(damage_type_id_of(*fall) == index_of("minecraft:fall"));
  CHECK(damage_type_by_id(damage_type_id_of(*fall)) == fall);
  CHECK(damage_has_tag(*fall, kDamageTagBypassesArmor));
  CHECK(damage_has_tag(*fall, kDamageTagIsFall));
  CHECK(fall->death_message_type == kDeathMessageFallVariants);

  // Tags are flattened, including nested references: panic_causes includes
  // is_player_attack, so player_attack carries both.
  const GeneratedDamageType* attack = damage_type_by_name("minecraft:player_attack");
  CHECK(attack != nullptr);
  CHECK(damage_has_tag(*attack, kDamageTagIsPlayerAttack));
  CHECK(damage_has_tag(*attack, kDamageTagPanicCauses));
  CHECK(damage_has_tag(*attack, kDamageTagCanBreakArmorStand));
  CHECK(!damage_has_tag(*attack, kDamageTagBypassesArmor));
  CHECK_NEAR(attack->exhaustion, 0.1, 1e-6);

  const GeneratedDamageType* kill = damage_type_by_name("generic_kill");
  CHECK(kill != nullptr);
  CHECK(damage_has_tag(*kill, kDamageTagBypassesInvulnerability));
  CHECK(damage_has_tag(*kill, kDamageTagBypassesResistance));

  // The registry is data: an unknown name is a null, never a silent 0.
  CHECK(damage_type_by_name("minecraft:not_a_damage_type") == nullptr);
  CHECK(damage_type_by_id(kDamageTypeCount) == nullptr);
  CHECK(damage_type_by_id(-1) == nullptr);

  // minecraft:attribute is *static*: the client keeps its own copy, so these
  // ids have to be vanilla's registration order.
  int32_t max_health = -1, armor = -1, toughness = -1, safe_fall = -1;
  for (int i = 0; i < kAttributeCount; ++i) {
    const std::string name = kAttributes[i].name;
    if (name == "minecraft:max_health")
      max_health = kAttributes[i].id;
    else if (name == "minecraft:armor")
      armor = kAttributes[i].id;
    else if (name == "minecraft:armor_toughness")
      toughness = kAttributes[i].id;
    else if (name == "minecraft:safe_fall_distance")
      safe_fall = kAttributes[i].id;
  }
  CHECK(kAttributeCount == 40);
  CHECK(max_health == 23);
  CHECK(armor == 1);
  CHECK(toughness == 2);
  CHECK(safe_fall == 29);
}

void test_armor() {
  ItemStack slots[4];
  armour_set(slots, "minecraft:diamond_boots", "minecraft:diamond_leggings",
             "minecraft:diamond_chestplate", "minecraft:diamond_helmet");
  const ArmorValues full = armor_values(slots, 4);
  CHECK_NEAR(full.points, 3 + 6 + 8 + 3, 1e-6); // 20
  CHECK_NEAR(full.toughness, 8.0, 1e-6);

  armour_set(slots, "minecraft:leather_boots", nullptr, nullptr, nullptr);
  const ArmorValues boots = armor_values(slots, 4);
  CHECK_NEAR(boots.points, 1, 1e-6);
  CHECK_NEAR(boots.toughness, 0, 1e-6);

  armour_set(slots, nullptr, "minecraft:netherite_leggings", nullptr, nullptr);
  const ArmorValues netherite = armor_values(slots, 4);
  CHECK_NEAR(netherite.points, 6, 1e-6);
  CHECK_NEAR(netherite.toughness, 3, 1e-6);

  // Armour modifiers are per slot and only where the item carries the value.
  armour_set(slots, nullptr, nullptr, "minecraft:iron_chestplate", "minecraft:turtle_helmet");
  std::vector<ArmorModifier> points, toughness;
  armor_modifiers(slots, 4, &points, &toughness);
  CHECK(points.size() == 2);
  CHECK(points[0].id == "minecraft:armor.chest" && points[0].amount == 6.0);
  CHECK(points[1].id == "minecraft:armor.head" && points[1].amount == 2.0);
  CHECK(toughness.empty()); // iron and turtle have no toughness

  // CombatRules.getDamageAfterAbsorb.
  CHECK_NEAR(damage_after_absorb(10.0f, 0.0f, 0.0f), 10.0f, 1e-5);
  // Full diamond: 20 armour, toughness 8 -> 2 + 2 = 4; 20 - 10/4 = 17.5 of 25
  // is 0.7, so 30% gets through.
  CHECK_NEAR(damage_after_absorb(10.0f, 20.0f, 8.0f), 10.0f - 10.0f * (17.5f / 25.0f), 1e-4);
  // The clamp's *floor* is a fifth of the raw armour, so at 20 armour a huge
  // hit still lands 84%: realArmor = max(20 - 1000/2, 20 * 0.2) = 4.
  CHECK_NEAR(damage_after_absorb(1000.0f, 20.0f, 0.0f), 1000.0f * 0.84f, 1e-2);
  // 25 armour is where the 20 cap binds instead: realArmor = max(25 - 500, 5) =
  // 5, i.e. 80% absorbed — the most armour can ever take off a single hit.
  CHECK_NEAR(damage_after_absorb(1000.0f, 25.0f, 0.0f), 1000.0f * 0.8f, 1e-2);

  // BYPASSES_ARMOR types ignore all of it.
  ArmorValues twenty;
  twenty.points = 20.0f;
  twenty.toughness = 8.0f;
  CHECK_NEAR(damage_after_armor(*damage_type_by_name("fall"), 10.0f, twenty), 10.0f, 1e-5);
  CHECK_NEAR(damage_after_armor(*damage_type_by_name("player_attack"), 10.0f, twenty), 3.0f, 1e-3);
  CHECK_NEAR(combat_rules_magic_absorb(10.0f, 20.0f), 2.0f, 1e-5);
}

// Player.hurtArmor -> LivingEntity.doHurtEquipment: the amount one landed hit
// charges each worn piece, and the number it is charged on — `DamageResult
// .landed`, the window-reduced damage rather than the caller's argument.
void test_armor_durability() {
  // `(int)Math.max(1.0f, damage / 4.0f)`, with the method's own `damage <= 0`
  // guard and Java's saturating float -> int cast.
  CHECK(armor_durability_damage(0.0f) == 0);
  CHECK(armor_durability_damage(-1.0f) == 0);
  CHECK(armor_durability_damage(0.5f) == 1);
  CHECK(armor_durability_damage(1.0f) == 1);
  CHECK(armor_durability_damage(4.0f) == 1);
  CHECK(armor_durability_damage(5.0f) == 1); // 1.25 truncates, like the cast
  CHECK(armor_durability_damage(7.999f) == 1);
  CHECK(armor_durability_damage(8.0f) == 2);
  CHECK(armor_durability_damage(11.999f) == 2);
  CHECK(armor_durability_damage(12.0f) == 3);
  CHECK(armor_durability_damage(20.0f) == 5);                  // point-blank TNT
  CHECK(armor_durability_damage(3.4028235e38f) == 2147483647); // Java's cast saturates

  // What the wear is charged on: the whole hit when it lands whole...
  const GeneratedDamageType& attack = *damage_type_by_name("player_attack");
  const SurvivalContext bare = context_with(nullptr);
  SurvivalState state;
  const DamageResult first = hit(state, attack, 5.0f, bare);
  CHECK(first.accepted);
  CHECK_NEAR(first.landed, 5.0f, 1e-5);

  // ...and the excess over the previous hit when the invulnerability window
  // clips it: that is the number actuallyHurt hands getDamageAfterArmorAbsorb,
  // and so the number the worn pieces are charged.
  const DamageResult second = hit(state, attack, 8.0f, bare);
  CHECK(second.accepted && second.reduced);
  CHECK_NEAR(second.landed, 3.0f, 1e-5);

  // A hit the window refuses outright lands nothing, so nothing wears.
  CHECK(!hit(state, attack, 2.0f, bare).accepted);

  // A shield that takes the whole hit leaves `landed` at zero, which is exactly
  // why a fully blocked hit wears nothing (`doHurtEquipment`'s `damage <= 0`
  // early return).
  SurvivalContext blocking = bare;
  blocking.shield_blocking = true;
  SurvivalState blocked;
  const DamageResult guarded = hit(blocked, attack, 20.0f, blocking);
  CHECK(guarded.accepted);
  CHECK_NEAR(guarded.landed, 0.0f, 1e-6);
}

void test_damage_pipeline() {
  const GeneratedDamageType& attack = *damage_type_by_name("player_attack");
  ItemStack bare[4];
  armour_set(bare, nullptr, nullptr, nullptr, nullptr);
  const SurvivalContext unarmoured = context_with(bare);

  SurvivalState state;
  DamageResult r = hit(state, attack, 5.0f, unarmoured);
  CHECK(r.accepted);
  CHECK_NEAR(r.applied, 5.0f, 1e-5);
  CHECK_NEAR(state.health, 15.0f, 1e-5);
  CHECK(!r.died);
  // Player.actuallyHurt charges the food bar for the damage that landed.
  CHECK_NEAR(state.exhaustion, 0.1f, 1e-5);
  CHECK_NEAR(state.invulnerable_time, 20.0f, 1e-5);
  CHECK_NEAR(state.last_hurt, 5.0f, 1e-5);

  // The invulnerability window: inside the first ten ticks only the excess of
  // the previous hit lands, and a smaller hit is refused outright.
  CHECK(!hit(state, attack, 3.0f, unarmoured).accepted);
  CHECK_NEAR(state.health, 15.0f, 1e-5);
  const DamageResult third = hit(state, attack, 8.0f, unarmoured);
  CHECK(third.accepted && third.reduced);
  CHECK_NEAR(third.applied, 3.0f, 1e-5); // 8 - 5
  CHECK_NEAR(state.health, 12.0f, 1e-5);

  // Once the window has run out a smaller hit lands in full.
  state.invulnerable_time = 0.0f;
  state.last_hurt = 0.0f;
  const DamageResult fourth = hit(state, attack, 1.0f, unarmoured);
  CHECK(fourth.accepted && !fourth.reduced);
  CHECK_NEAR(fourth.applied, 1.0f, 1e-5);

  // Armour turns the same hit down.
  SurvivalState armoured;
  ItemStack diamond[4];
  armour_set(diamond, "minecraft:diamond_boots", "minecraft:diamond_leggings",
             "minecraft:diamond_chestplate", "minecraft:diamond_helmet");
  const DamageResult plated = hit(armoured, attack, 10.0f, context_with(diamond));
  CHECK(plated.accepted);
  CHECK_NEAR(plated.applied, 3.0f, 1e-3); // 10 * (1 - 17.5/25)
  CHECK_NEAR(armoured.health, 17.0f, 1e-3);

  // Absorption hearts eat the damage before the health bar. The ceiling matters:
  // `LivingEntity.setAbsorptionAmount` clamps to the MAX_ABSORPTION attribute,
  // whose base is 0, so absorption only exists alongside the modifier the
  // Absorption effect contributes.
  SurvivalState absorbed;
  SurvivalContext with_absorption = unarmoured;
  with_absorption.max_absorption = 4.0f;
  absorbed.absorption = 4.0f;
  const DamageResult a = hit(absorbed, attack, 3.0f, with_absorption);
  CHECK(a.accepted);
  CHECK_NEAR(a.applied, 0.0f, 1e-5);
  CHECK_NEAR(a.absorbed, 3.0f, 1e-5);
  CHECK_NEAR(absorbed.health, 20.0f, 1e-5);
  CHECK_NEAR(absorbed.absorption, 1.0f, 1e-5);

  // ...and the ceiling is applied *after* the hearts are spent, exactly where
  // LivingEntity.actuallyHurt puts `setAbsorptionAmount` (which is what
  // clamps). A starting absorption above the ceiling is unreachable in play,
  // because every write goes through that same clamp; the point here is that
  // the clamp never turns spent absorption back into health.
  SurvivalState no_ceiling;
  no_ceiling.absorption = 4.0f;
  const DamageResult clamped = hit(no_ceiling, attack, 3.0f, unarmoured);
  CHECK(clamped.accepted);
  CHECK_NEAR(clamped.applied, 0.0f, 1e-5);
  CHECK_NEAR(clamped.absorbed, 3.0f, 1e-5);
  CHECK_NEAR(no_ceiling.absorption, 0.0f, 1e-5);

  // Creative (abilities.invulnerable) takes nothing except the types that
  // bypass invulnerability — which is exactly what /kill uses.
  SurvivalContext creative;
  creative.invulnerable = true;
  SurvivalState god;
  CHECK(!hit(god, attack, 100.0f, creative).accepted);
  CHECK_NEAR(god.health, 20.0f, 1e-5);
  const DamageResult killed =
      hit(god, *damage_type_by_name("generic_kill"), 3.4028235e38f, creative);
  CHECK(killed.accepted && killed.died);
  CHECK_NEAR(god.health, 0.0f, 1e-5);
  CHECK(god.dead);

  // A dead player takes nothing more.
  CHECK(!hit(god, attack, 5.0f, unarmoured).accepted);
}

void test_food_tick() {
  const SurvivalContext ctx;
  SurvivalState fresh;
  const SurvivalTick idle = tick_food(fresh, ctx);
  CHECK(!idle.changed);
  CHECK(!idle.starve_due);

  // Four exhaustion burns one saturation point, one point per tick.
  SurvivalState fed;
  fed.exhaustion = 12.0f;
  tick_food(fed, ctx);
  CHECK_NEAR(fed.exhaustion, 8.0f, 1e-5);
  CHECK_NEAR(fed.saturation, 4.0f, 1e-5);
  CHECK(fed.food == 20);

  // With no saturation left the food level drops...
  SurvivalState hungry;
  hungry.saturation = 0.0f;
  hungry.exhaustion = 5.0f;
  tick_food(hungry, ctx);
  CHECK(hungry.food == 19);
  CHECK_NEAR(hungry.exhaustion, 1.0f, 1e-5);

  // ... but not on peaceful, where food never drains.
  SurvivalContext peaceful;
  peaceful.difficulty = kDifficultyPeaceful;
  SurvivalState calm;
  calm.saturation = 0.0f;
  calm.exhaustion = 5.0f;
  tick_food(calm, peaceful);
  CHECK(calm.food == 20);
  CHECK_NEAR(calm.exhaustion, 1.0f, 1e-5);

  // The fast bar: a hurt player at full food spends up to six saturation to
  // heal 1/6 of a heart every ten ticks.
  SurvivalState regen;
  regen.health = 10.0f;
  for (int i = 0; i < 9; ++i)
    tick_food(regen, ctx);
  CHECK_NEAR(regen.health, 10.0f, 1e-5); // the tenth tick is the one that heals
  tick_food(regen, ctx);
  CHECK_NEAR(regen.health, 10.0f + 5.0f / 6.0f, 1e-4);
  // The heal is paid for in *exhaustion* (FoodData.tick calls addExhaustion(f)),
  // not in saturation directly: the bar is still full here, and the next tick
  // is what turns a whole 4 exhaustion points into one saturation point.
  CHECK_NEAR(regen.exhaustion, 5.0f, 1e-5);
  CHECK_NEAR(regen.saturation, 5.0f, 1e-5);
  tick_food(regen, ctx);
  CHECK_NEAR(regen.exhaustion, 1.0f, 1e-5);
  CHECK_NEAR(regen.saturation, 4.0f, 1e-5);
  CHECK(regen.food == 20);

  // The slow bar: food >= 18 heals a heart every 80 ticks for six exhaustion.
  SurvivalState slow;
  slow.health = 10.0f;
  slow.saturation = 0.0f;
  slow.food = 18;
  for (int i = 0; i < 79; ++i)
    tick_food(slow, ctx);
  CHECK_NEAR(slow.health, 10.0f, 1e-5);
  tick_food(slow, ctx);
  CHECK_NEAR(slow.health, 11.0f, 1e-5);
  CHECK_NEAR(slow.exhaustion, 6.0f, 1e-5);

  // Starvation: food 0 bites every 80 ticks, and on normal difficulty only
  // above half a heart.
  SurvivalState starving;
  starving.saturation = 0.0f;
  starving.food = 0;
  bool due = false;
  for (int i = 0; i < 80; ++i)
    due = tick_food(starving, ctx).starve_due || due;
  CHECK(due);

  SurvivalState fragile;
  fragile.saturation = 0.0f;
  fragile.food = 0;
  fragile.health = 1.0f;
  for (int i = 0; i < 80; ++i)
    CHECK(!tick_food(fragile, ctx).starve_due);

  SurvivalContext hard;
  hard.difficulty = kDifficultyHard;
  SurvivalState hard_case;
  hard_case.saturation = 0.0f;
  hard_case.food = 0;
  hard_case.health = 1.0f;
  bool hard_due = false;
  for (int i = 0; i < 80; ++i)
    hard_due = tick_food(hard_case, hard).starve_due || hard_due;
  CHECK(hard_due);
}

void test_air() {
  const SurvivalContext dry;
  SurvivalState state;
  CHECK(state.air == kMaxAirSupply);
  // Out of the water the supply refills four ticks at a time.
  state.air = 100;
  tick_air(state, dry, 0.0);
  CHECK(state.air == 104);
  state.air = kMaxAirSupply - 1;
  tick_air(state, dry, 0.0);
  CHECK(state.air == kMaxAirSupply);

  // Underwater it drains one a tick, and drowning only bites once the supply is
  // 20 past zero (LivingEntity.shouldTakeDrowningDamage).
  SurvivalContext wet;
  wet.eye_in_water = true;
  SurvivalState under;
  under.air = 1;
  CHECK(!tick_air(under, wet, 0.0)); // 0 air left
  for (int i = 0; i < 19; ++i)
    CHECK(!tick_air(under, wet, 0.0)); // -1 .. -19
  CHECK(tick_air(under, wet, 0.0));    // -20: the damage tick
  CHECK(under.air == 0);               // and the supply resets rather than stays negative

  // A water-breathing (or creative) player never drowns.
  SurvivalContext breathing = wet;
  breathing.can_breathe_underwater = true;
  SurvivalState safe;
  safe.air = 0;
  for (int i = 0; i < 100; ++i)
    CHECK(!tick_air(safe, breathing, 0.0));
  CHECK(safe.air == 0);

  // oxygen_bonus turns the drain into a chance: a lucky roll keeps the air.
  SurvivalContext lungs = wet;
  lungs.oxygen_bonus = 3.0; // skip chance 1/4
  SurvivalState lucky;
  lucky.air = 100;
  tick_air(lucky, lungs, 0.9);
  CHECK(lucky.air == 100);
  tick_air(lucky, lungs, 0.1);
  CHECK(lucky.air == 99);
}

void test_fall() {
  SurvivalState state;
  CHECK_NEAR(spend_fall_distance(state, -4.0, false, false), 0.0, 1e-9);
  CHECK_NEAR(state.fall_distance, 4.0, 1e-9);
  CHECK_NEAR(spend_fall_distance(state, -5.0, true, false), 9.0, 1e-9);
  CHECK_NEAR(state.fall_distance, 0.0, 1e-9);
  CHECK_NEAR(spend_fall_distance(state, 0.0, true, false), 0.0, 1e-9);

  // LivingEntity.calculateFallDamage: floor(fallDistance - safeFallDistance).
  CHECK(fall_damage(3.0, 3.0, 1.0, false) == 0);
  CHECK(fall_damage(4.0, 3.0, 1.0, false) == 1);
  CHECK(fall_damage(23.5, 3.0, 1.0, false) == 20);
  CHECK(fall_damage(100.0, 3.0, 1.0, true) == 0); // creative flying: none at all

  // Through the real pipeline: a fall ignores armour (bypasses_armor), a punch
  // does not.
  ItemStack leggings[4];
  armour_set(leggings, nullptr, "minecraft:diamond_leggings", nullptr, nullptr);
  SurvivalState faller;
  hit(faller, *damage_type_by_name("fall"), 10.0f, context_with(leggings));
  CHECK_NEAR(faller.health, 10.0f, 1e-5);
  // Diamond leggings are 6 armour points *and* 2 toughness, so the effective
  // armour is clamp(6 - 10/2.5, 1.2, 20) = 2 and 9.2 of the 10 damage lands.
  SurvivalState punched;
  hit(punched, *damage_type_by_name("player_attack"), 10.0f, context_with(leggings));
  CHECK_NEAR(punched.health, 20.0f - damage_after_absorb(10.0f, 6.0f, 2.0f), 1e-3);
}

void test_eat() {
  SurvivalState full;
  CHECK(!can_eat(full, false, false));
  CHECK(can_eat(full, true, false)); // golden apple
  CHECK(can_eat(full, false, true)); // creative: Player.canEat's abilities.invulnerable
  full.food = 19;
  CHECK(can_eat(full, false, false));

  SurvivalState hungry;
  hungry.food = 10;
  hungry.saturation = 0.0f;
  eat(hungry, 8, 12.8f);
  CHECK(hungry.food == 18);
  CHECK_NEAR(hungry.saturation, 12.8f, 1e-4);
  // Saturation is capped at the food level (FoodData.add).
  eat(hungry, 8, 12.8f);
  CHECK(hungry.food == 20);
  CHECK_NEAR(hungry.saturation, 20.0f, 1e-4);

  // The item table's food columns are what the eat path reads.
  const ItemRegistry& items = ItemRegistry::instance();
  const GeneratedItem* beef = items.by_id(item("minecraft:cooked_beef"));
  CHECK(beef->nutrition == 8);
  CHECK_NEAR(beef->saturation, 12.8f, 1e-5);
  CHECK(beef->consume_ticks == 32); // 1.6 s, Consumable's default
  CHECK(items.by_id(item("minecraft:dried_kelp"))->consume_ticks == 16);
  // Food but no consumable: the four fish buckets are not edible.
  CHECK(items.by_id(item("minecraft:cod_bucket"))->consume_ticks == 0);
  CHECK((items.by_id(item("minecraft:apple"))->flags & kItemCanAlwaysEat) == 0);
  CHECK((items.by_id(item("minecraft:golden_apple"))->flags & kItemCanAlwaysEat) != 0);

  SurvivalState hurt;
  hurt.health = 19.0f;
  heal(hurt, 5.0f, 20.0f);
  CHECK_NEAR(hurt.health, 20.0f, 1e-5);
}

void test_health_packet_guard() {
  // ServerPlayer.doTick's resume rule: health or food moved, or saturation
  // crossed zero — not saturation itself.
  SurvivalState state;
  state.mark_health_sent();
  CHECK(!state.health_packet_due());
  state.saturation = 3.0f;
  CHECK(!state.health_packet_due());
  state.saturation = 0.0f;
  CHECK(state.health_packet_due()); // crossed zero
  state.mark_health_sent();
  CHECK(!state.health_packet_due());
  state.health = 19.0f;
  CHECK(state.health_packet_due());
  state.mark_health_sent();
  state.food = 19;
  CHECK(state.health_packet_due());
}

void test_death_message() {
  const GeneratedDamageType& fall = *damage_type_by_name("fall");
  DeathMessage message = death_message_for(source_of(fall), "Steve");
  CHECK(message.key == "death.fell.accident.generic"); // fall_variants, no attacker
  CHECK(message.victim == "Steve");
  CHECK(!message.has_attacker);

  DamageSource assisted = source_of(fall);
  assisted.causing_entity_id = 7;
  assisted.causing_name = "Alex";
  message = death_message_for(assisted, "Steve");
  CHECK(message.key == "death.fell.finish");
  CHECK(message.has_attacker && message.attacker == "Alex");

  message = death_message_for(source_of(*damage_type_by_name("starve")), "Steve");
  CHECK(message.key == "death.attack.starve");

  // A cause entity — a player or a mob — uses the *bare* key with the attacker
  // as the second argument. `death.attack.player.player` and
  // `death.attack.mob.player` are not lang keys vanilla ever asks for
  // (DamageSource.getLocalizedDeathMessage), and the old rule here invented
  // `death.attack.mob.player` for every mob kill, which the client renders as
  // the raw key.
  DamageSource by_player = source_of(*damage_type_by_name("player_attack"));
  by_player.causing_entity_id = 3;
  by_player.causing_name = "Alex";
  message = death_message_for(by_player, "Steve");
  CHECK(message.key == "death.attack.player");
  CHECK(message.has_attacker && message.attacker == "Alex");

  DamageSource by_mob = source_of(*damage_type_by_name("mob_attack"));
  by_mob.causing_entity_id = 11;
  by_mob.causing_name = "Zombie";
  message = death_message_for(by_mob, "Steve");
  CHECK(message.key == "death.attack.mob");
  CHECK(message.has_attacker && message.attacker == "Zombie");

  // No cause on the killing blow, but a kill credit: the `.player` fallback,
  // which is the *only* place vanilla appends that suffix (`death.fell.*` is
  // the other branch, above).
  message = death_message_for(source_of(*damage_type_by_name("generic")), "Steve", "Zombie");
  CHECK(message.key == "death.attack.generic.player");
  CHECK(message.has_attacker && message.attacker == "Zombie");
  // ... and with no credit, only the victim is named.
  message = death_message_for(source_of(*damage_type_by_name("generic")), "Steve");
  CHECK(message.key == "death.attack.generic");
  CHECK(!message.has_attacker);
}

void test_packets() {
  using namespace mc::server;
  // set_health: [f32 health][varint food][f32 saturation].
  const std::vector<uint8_t> health = make_set_health(19.5f, 17, 3.5f);
  CHECK(health.size() == 10);
  CHECK(health[0] == 0x6a);
  CHECK(health[1] == 0x41 && health[2] == 0x9C); // 19.5
  CHECK(health[5] == 17);
  CHECK(health[6] == 0x40 && health[7] == 0x60); // 3.5

  // damage_event, with the +1 optional entity ids and the optional position.
  const std::vector<uint8_t> bare = make_damage_event(9, 10, -1, -1);
  CHECK(bare.size() == 6);
  CHECK(bare[0] == 0x19 && bare[1] == 9 && bare[2] == 10);
  CHECK(bare[3] == 0 && bare[4] == 0 && bare[5] == 0);
  const std::vector<uint8_t> with_cause = make_damage_event(9, 10, 4, 4, true, 1.0, 2.0, 3.0);
  CHECK(with_cause.size() == 6 + 24);
  CHECK(with_cause[3] == 5); // cause 4 -> 5
  CHECK(with_cause[5] == 1); // has position

  // entity_event: a *fixed* 4-byte entity id, the one packet here that is not
  // varint-prefixed.
  const std::vector<uint8_t> event = make_entity_event(300, 3);
  CHECK(event.size() == 6);
  CHECK(event[0] == 0x22);
  CHECK(event[1] == 0 && event[2] == 0 && event[3] == 0x01 && event[4] == 0x2C);
  CHECK(event[5] == 3);

  // hurt_animation: [varint id][f32 yaw].
  const std::vector<uint8_t> hurt = make_hurt_animation(4);
  CHECK(hurt.size() == 6);
  CHECK(hurt[0] == 0x2b && hurt[1] == 4);

  // update_attributes with an armour modifier. The packet id is 0x86, which is
  // *two* varint bytes (131), so every offset below counts from after them.
  // Layout: [0..1] id, [2] entity, [3] count, [4] attribute id, [5..12] base
  // double, [13] modifier count, [14] identifier length, [15..35] identifier,
  // [36..43] amount double, [44] operation varint.
  AttributeValue armor;
  armor.id = 1;
  armor.base = 0.0;
  AttributeModifierValue modifier;
  modifier.id = "minecraft:armor.chest";
  modifier.amount = 8.0;
  armor.modifiers.push_back(modifier);
  const std::vector<uint8_t> attributes = make_update_attributes(5, {armor});
  CHECK(attributes.size() == 45);
  CHECK(attributes[0] == 0x86 && attributes[1] == 0x01); // varint 134
  CHECK(attributes[2] == 5 && attributes[3] == 1);
  CHECK(attributes[4] == 1);     // minecraft:armor
  CHECK(attributes[13] == 1);    // one modifier
  CHECK(attributes[14] == 0x15); // identifier length 21
  CHECK_BYTES(attributes, 15, "minecraft:armor.chest");
  CHECK(attributes[36] == 0x40 && attributes[37] == 0x20); // 8.0
  CHECK(attributes[44] == 0);                              // Operation.ADD_VALUE

  // player_combat_kill: [varint id][component as network NBT]. The component is
  // a TAG_Compound with a "translate" string, which is what puts the client on
  // the death screen (ClientPacketListener.handlePlayerCombatKill).
  const std::vector<uint8_t> kill = make_player_combat_kill(6, "death.attack.starve", {"Steve"});
  CHECK(kill[0] == 0x45 && kill[1] == 6);
  CHECK(kill[2] == 0x0A);                    // TAG_Compound
  CHECK(kill[3] == 0x08);                    // TAG_String for the key
  CHECK(kill[4] == 0x00 && kill[5] == 0x09); // the name "translate" (9 bytes)
  CHECK_BYTES(kill, 6, "translate");
  CHECK(kill[15] == 0x00 && kill[16] == 0x13); // the key value: 19 bytes
  CHECK_BYTES(kill, 17, "death.attack.starve");
  CHECK(kill[36] == 0x09); // the "with" list
  CHECK(kill.back() == 0x00);

  // respawn: the spawn info then dataToKeep.
  CommonSpawnInfo info;
  info.dimension_type_id = 1;
  info.dimension = "minecraft:overworld";
  info.game_mode = 0;
  info.is_flat = true;
  const std::vector<uint8_t> respawn =
      make_respawn(info, mc::protocol::kRespawnKeepAttributeModifiers);
  CHECK(respawn[0] == 0x54 && respawn[1] == 1);
  CHECK(respawn[2] == 19); // varint length of "minecraft:overworld"
  CHECK_BYTES(respawn, 3, "minecraft:overworld");
  CHECK(respawn.back() == 1);

  // change_difficulty: [u8 difficulty][bool locked].
  const std::vector<uint8_t> difficulty = make_change_difficulty(2, false);
  CHECK(difficulty.size() == 3 && difficulty[0] == 0x0A && difficulty[1] == 2);

  // set_equipment: the continuation bit on every entry but the last, with the
  // EquipmentSlot ordinals.
  const std::vector<uint8_t> equipment =
      make_set_equipment(7, {{kEquipmentChest, stack("minecraft:iron_chestplate")},
                             {kEquipmentHead, stack("minecraft:iron_helmet")}});
  CHECK(equipment[0] == 0x68 && equipment[1] == 7);
  CHECK(equipment[2] == (kEquipmentChest | 0x80));
  CHECK((equipment[2] & 0x7F) == 4);
  // The second entry's slot byte has no continuation bit; find it by walking.
  CHECK(equipment[2] == 0x84);
}

} // namespace

int main() {
  test_damage_table();
  test_armor();
  test_armor_durability();
  test_damage_pipeline();
  test_food_tick();
  test_air();
  test_fall();
  test_eat();
  test_health_packet_guard();
  test_death_message();
  test_packets();
  if (failures == 0)
    std::printf("survival_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
