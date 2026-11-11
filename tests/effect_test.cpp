// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for status effects (Phase 3 milestone G):
//   * the generated registry: 40 effects, the table index *is* the protocol id,
//     and the cases whose data is code-only come out right (Speed's colour and
//     movement-speed modifier, Instant Health's harm flag, Darkness's blend,
//     Fire Resistance's 0xFF9900),
//   * MobEffect.shouldApplyEffectTickThisTick per class: Regeneration's
//     50 >> amplifier cadence, Poison's 25, Wither's 40, Hunger's every tick,
//     the plain class's never, and an instantaneous effect's `duration >= 1`,
//   * ActiveEffects.add: the first instance reports as *added*, a stronger or
//     longer repeat merges and reports as *updated*, and a weaker or shorter one
//     is refused outright (MobEffectInstance.update only strengthens),
//   * the hidden-effect chain: a short strong instance sitting on top of a long
//     weak one, and the weak one coming back when the strong one expires,
//   * the tick: Regeneration only while health is missing, Poison's `health > 1`
//     floor, Wither's lack of one, Hunger's exhaustion, Saturation's eat,
//     Instant Health firing once and going, and Absorption removing itself once
//     the hearts are gone,
//   * ListEntities' 600-tick refresh (LivingEntity.tickEffects' `duration % 600`),
//   * the attribute modifiers: the per-level scaling, the vanilla modifier id,
//     AttributeInstance.calculateValue's ordering, and Resistance's reduction.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/effect.h"

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

MobEffectInstance make(const char* name, int duration, int amplifier = 0) {
  MobEffectInstance instance;
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  instance.effect = def ? mob_effect_id_of(*def) : -1;
  instance.duration = duration;
  instance.amplifier = amplifier;
  return instance;
}

// The registry id of an effect by name, so a test never has to spell an index
// out twice — the registry's order is the wire's, not this file's.
int32_t id_of(const char* name) {
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  return def ? mob_effect_id_of(*def) : -1;
}

// The five registry events of one outcome, as a single comparable string, so a
// test can say what happened in one assertion.
std::string describe(const EffectOutcome& outcome) {
  std::string text;
  for (int32_t id : outcome.added)
    text += " +" + std::to_string(id);
  for (int32_t id : outcome.updated)
    text += " ~" + std::to_string(id);
  for (int32_t id : outcome.removed)
    text += " -" + std::to_string(id);
  for (const EffectEvent& event : outcome.events) {
    text += " e" + std::to_string(static_cast<int>(event.kind)) + "@" +
            std::to_string(static_cast<int>(event.amount * 1000.0f));
  }
  return text;
}

void test_registry() {
  CHECK(kMobEffectCount == 40);
  // The table index is the protocol id an update_mob_effect packet carries, so
  // the ids must be dense and in order — the generator checks this against the
  // data generator's report, and this pins that it did.
  for (int i = 0; i < kMobEffectCount; ++i) {
    const GeneratedMobEffect* def = mob_effect_by_id(i);
    CHECK(def != nullptr);
    CHECK(mob_effect_id_of(*def) == i);
  }
  CHECK(mob_effect_by_id(-1) == nullptr);
  CHECK(mob_effect_by_id(kMobEffectCount) == nullptr);

  // Names normalize the way every other registry lookup in the engine does.
  const GeneratedMobEffect* speed = mob_effect_by_name("speed");
  CHECK(speed != nullptr);
  CHECK(mob_effect_id_of(*speed) == 0);
  CHECK(mob_effect_by_name("minecraft:speed") == speed);
  CHECK(mob_effect_by_name("SPEED") == speed);
  CHECK(mob_effect_by_name("not an effect") == nullptr);

  // Speed: beneficial, no per-tick behaviour, and +20% movement speed per level
  // as an ADD_MULTIPLIED_TOTAL modifier (which is what makes Speed II +40%).
  CHECK(std::string(speed->name) == "minecraft:speed");
  CHECK(speed->category == kEffectCategoryBeneficial);
  CHECK(speed->kind == kEffectPlain);
  CHECK(!speed->instantaneous);
  CHECK(speed->color == 3402751u);
  CHECK(speed->modifier_count == 1);
  CHECK(std::string(speed->modifiers[0].attribute) == "minecraft:movement_speed");
  CHECK_NEAR(speed->modifiers[0].amount, 0.2f, 1e-6);
  CHECK(speed->modifiers[0].operation == kModifierAddMultipliedTotal);

  // Fire Resistance's colour is written as a hex literal in MobEffects.java, so
  // this is the case that would break if the parser only handled decimals.
  const GeneratedMobEffect* fire_resistance = mob_effect_by_name("fire_resistance");
  CHECK(fire_resistance != nullptr);
  CHECK(fire_resistance->color == 0xFF9900u);

  // Instant Health and Instant Damage are the same class with the harm flag
  // flipped, and both are instantaneous.
  const GeneratedMobEffect* heal = mob_effect_by_name("instant_health");
  const GeneratedMobEffect* harm = mob_effect_by_name("instant_damage");
  CHECK(heal && harm);
  CHECK(heal->instantaneous && harm->instantaneous);
  CHECK(!heal->harm && harm->harm);
  CHECK(heal->kind == kEffectInstantHealthOrHarm && harm->kind == kEffectInstantHealthOrHarm);
  CHECK(std::string(heal->name) == "minecraft:instant_health");

  // Darkness and Nausea carry the blend durations, both spellings of the builder
  // call (one argument and three).
  const GeneratedMobEffect* darkness = mob_effect_by_name("darkness");
  CHECK(darkness && darkness->blend_in == 22 && darkness->blend_out == 22 &&
        darkness->blend_out_advance == 22);
  const GeneratedMobEffect* nausea = mob_effect_by_name("nausea");
  CHECK(nausea && nausea->blend_in == 150 && nausea->blend_out == 20 &&
        nausea->blend_out_advance == 60);

  // The effect classes a future version could add beyond these would fall on
  // `plain`, which is what MobEffect itself is.
  CHECK(mob_effect_by_name("absorption")->kind == kEffectAbsorption);
  CHECK(mob_effect_by_name("regeneration")->kind == kEffectRegeneration);
  CHECK(mob_effect_by_name("poison")->kind == kEffectPoison);
  CHECK(mob_effect_by_name("wither")->kind == kEffectWither);
  CHECK(mob_effect_by_name("hunger")->kind == kEffectHunger);
  CHECK(mob_effect_by_name("saturation")->kind == kEffectSaturation);
}

void test_glowing_bit() {
  // `LivingEntity.updateGlowingStatus`'s answer over any entity's effect set:
  // the shared flags' bit 6 is `hasEffect(MobEffects.GLOWING)`. It is one
  // derivation for a mob and a player, so a set that ticks its own durations
  // (both do) cannot disagree with the flag the client is drawn from.
  const int32_t glowing = glowing_effect_id();
  CHECK(glowing >= 0);
  CHECK(glowing_effect_id() == glowing); // resolved once, stable after
  CHECK(mob_effect_by_name("minecraft:glowing") == mob_effect_by_id(glowing));
  // Glowing is a *plain* effect: its tick is a no-op, so the bit is the whole of
  // what it does, and it is not instantaneous (a duration-1 instance from
  // `/effect give` is ticked, not applied).
  CHECK(mob_effect_by_id(glowing) != nullptr && !mob_effect_by_id(glowing)->instantaneous);
  CHECK(mob_effect_by_id(glowing)->kind == kEffectPlain);

  ActiveEffects effects;
  CHECK(!effects_glow(effects)); // an empty set is both bits clear
  EffectOutcome outcome;
  MobEffectInstance speed;
  speed.effect = mob_effect_id_of(*mob_effect_by_name("minecraft:speed"));
  speed.duration = 100;
  CHECK(effects.add(speed, outcome));
  CHECK(!effects_glow(effects)); // some other effect is not a glow

  MobEffectInstance glow;
  glow.effect = glowing;
  glow.duration = 200; // SpectralArrow.DEFAULT_DURATION
  CHECK(effects.add(glow, outcome));
  CHECK(effects_glow(effects));
  // A merge that only refreshes the duration keeps the bit...
  glow.duration = 400;
  CHECK(effects.add(glow, outcome));
  CHECK(effects_glow(effects));
  // ...and removing the instance takes it with it, leaving the rest of the set.
  CHECK(effects.remove(glowing));
  CHECK(!effects_glow(effects));
  CHECK(effects.has(speed.effect));

  // The tick that expires the last instance is the tick the bit falls on, which
  // is what lets a tick-driven entity (a mob) and a packet-driven one (a player)
  // both read it at the same point of their own tick.
  ActiveEffects ticking;
  MobEffectInstance brief;
  brief.effect = glowing;
  brief.duration = 1;
  CHECK(ticking.add(brief, outcome));
  CHECK(effects_glow(ticking));
  const EffectTickContext ctx{20.0f, 20.0f, 0.0f};
  ticking.tick(/*entity_tick_count=*/0, ctx, outcome);
  CHECK(ticking.size() == 0);
  CHECK(ticking.empty());
  CHECK(!effects_glow(ticking));
}

void test_invisibility_bit() {
  // `LivingEntity.updateInvisibilityStatus`: `setInvisible(hasEffect(MobEffects
  // .INVISIBILITY))`, the shared flags' flag 5 — and `setInvisible(false)` when
  // the set is empty. The same derivation shape as the glow above, over the same
  // container, and that is the point: the bit follows the set, so an expiry, a
  // `/effect clear` and a death take it down with nothing left to remember.
  ActiveEffects effects;
  CHECK(!effects_invisible(effects)); // the empty-set branch, one answer
  EffectOutcome outcome;
  MobEffectInstance speed;
  speed.effect = mob_effect_id_of(*mob_effect_by_name("minecraft:speed"));
  speed.duration = 100;
  CHECK(effects.add(speed, outcome));
  CHECK(!effects_invisible(effects)); // some other effect is not invisibility

  MobEffectInstance invisible;
  invisible.effect = mob_effect_id_of(*mob_effect_by_name("minecraft:invisibility"));
  invisible.duration = 200;
  CHECK(effects.add(invisible, outcome));
  CHECK(effects_invisible(effects));
  // A merge that only refreshes the duration keeps the bit...
  invisible.duration = 400;
  CHECK(effects.add(invisible, outcome));
  CHECK(effects_invisible(effects));
  // ...and the instance leaving takes it, leaving the rest of the set.
  CHECK(effects.remove(invisible.effect));
  CHECK(!effects_invisible(effects));
  CHECK(effects.has(speed.effect));
  // The two bits are one byte: a set that raises both raises both.
  MobEffectInstance glow;
  glow.effect = glowing_effect_id();
  glow.duration = 200;
  CHECK(effects.add(glow, outcome));
  CHECK(effects.add(invisible, outcome));
  CHECK(effects_glow(effects) && effects_invisible(effects));

  // The tick that expires the last instance is the tick the bit falls on — the
  // same property the glow has, so a tick-driven entity (a mob) and a
  // packet-driven one (a player) can both read it at the same point of a tick.
  ActiveEffects ticking;
  MobEffectInstance brief;
  brief.effect = invisible.effect;
  brief.duration = 1;
  CHECK(ticking.add(brief, outcome));
  CHECK(effects_invisible(ticking));
  const EffectTickContext ctx{20.0f, 20.0f, 0.0f};
  ticking.tick(/*entity_tick_count=*/0, ctx, outcome);
  CHECK(ticking.empty());
  CHECK(!effects_invisible(ticking));
}

void test_tick_predicate() {
  // RegenerationMobEffect: `50 >> amplification`, and the interval collapsing to
  // zero means every tick.
  CHECK(effect_should_apply_tick(kEffectRegeneration, 0, 0));
  CHECK(effect_should_apply_tick(kEffectRegeneration, 50, 0));
  CHECK(!effect_should_apply_tick(kEffectRegeneration, 51, 0));
  CHECK(effect_should_apply_tick(kEffectRegeneration, 25, 1)); // 50 >> 1 == 25
  CHECK(!effect_should_apply_tick(kEffectRegeneration, 24, 1));
  // 50 >> 6 == 0, so Regeneration VII fires every tick.
  CHECK(effect_should_apply_tick(kEffectRegeneration, 7, 6));

  // Poison: 25 >> amplifier; Wither: 40 >> amplifier.
  CHECK(effect_should_apply_tick(kEffectPoison, 25, 0));
  CHECK(!effect_should_apply_tick(kEffectPoison, 24, 0));
  CHECK(effect_should_apply_tick(kEffectPoison, 6, 2)); // 25 >> 2 == 6
  CHECK(effect_should_apply_tick(kEffectWither, 40, 0));
  CHECK(!effect_should_apply_tick(kEffectWither, 39, 0));
  CHECK(effect_should_apply_tick(kEffectWither, 20, 1));

  // Hunger and Absorption are `true` regardless of the tick count.
  CHECK(effect_should_apply_tick(kEffectHunger, 0, 0));
  CHECK(effect_should_apply_tick(kEffectHunger, 12345, 3));
  CHECK(effect_should_apply_tick(kEffectAbsorption, 0, 0));

  // The plain class has no tick, which is what MobEffect itself returns.
  CHECK(!effect_should_apply_tick(kEffectPlain, 0, 0));
  CHECK(!effect_should_apply_tick(kEffectPlain, 600, 4));

  // InstantaneousMobEffect: `remainingDuration >= 1`, so duration 1 fires once
  // and duration 0 never does.
  CHECK(effect_should_apply_tick(kEffectInstantHealthOrHarm, 1, 0));
  CHECK(!effect_should_apply_tick(kEffectInstantHealthOrHarm, 0, 0));
  CHECK(effect_should_apply_tick(kEffectSaturation, 1, 0));
}

void test_add_and_merge() {
  ActiveEffects effects;
  CHECK(effects.empty() && effects.size() == 0);

  EffectOutcome outcome;
  CHECK(effects.add(make("speed", 200, 0), outcome));
  CHECK(effects.size() == 1);
  // A brand-new instance is an `added` (onEffectAdded), never an `updated`.
  CHECK(describe(outcome) == " +0");

  // The same effect at a higher amplifier takes over, and the client is told to
  // update rather than add.
  outcome = EffectOutcome{};
  CHECK(effects.add(make("speed", 200, 1), outcome));
  CHECK(describe(outcome) == " ~0");
  CHECK(effects.get(0)->amplifier == 1);
  CHECK(effects.get(0)->duration == 200);

  // A longer duration at the *same* amplifier extends it.
  outcome = EffectOutcome{};
  CHECK(effects.add(make("speed", 400, 1), outcome));
  CHECK(effects.get(0)->duration == 400);

  // Neither weaker nor shorter: MobEffectInstance.update changes nothing, so
  // addEffect reports false and /effect's "nothing changed" refusal fires.
  outcome = EffectOutcome{};
  CHECK(!effects.add(make("speed", 100, 1), outcome));
  CHECK(describe(outcome).empty());
  CHECK(effects.get(0)->duration == 400);

  outcome = EffectOutcome{};
  CHECK(!effects.add(make("speed", 400, 0), outcome));
  CHECK(effects.get(0)->amplifier == 1);

  // An unknown effect id is refused rather than stored.
  MobEffectInstance bogus;
  bogus.effect = 9999;
  bogus.duration = 100;
  outcome = EffectOutcome{};
  CHECK(!effects.add(bogus, outcome));
  CHECK(effects.size() == 1);

  // Removal, and removal of something that is not there.
  CHECK(!effects.remove(1));
  CHECK(effects.remove(0));
  CHECK(effects.empty());

  // Absorption's onEffectStarted is the one immediate side effect: an
  // absorption event for 4 * (1 + amplifier), on top of the `added`.
  outcome = EffectOutcome{};
  CHECK(effects.add(make("absorption", 600, 1), outcome));
  CHECK(describe(outcome) == " +21 e4@8000");
  CHECK(effects.remove_all());
  CHECK(!effects.remove_all());
}

void test_hidden_effect_chain() {
  // A short strong instance on top of a long weak one: what is left when the
  // strong one runs out is the weak one, not nothing.
  ActiveEffects effects;
  EffectOutcome outcome;
  CHECK(effects.add(make("speed", -1, 0), outcome)); // infinite Speed I
  CHECK(effects.add(make("speed", 5, 2), outcome));  // Speed III for 5 ticks
  CHECK(effects.get(0)->amplifier == 2);
  CHECK(effects.get(0)->duration == 5);
  CHECK(effects.get(0)->hidden.size() == 1);
  CHECK(effects.get(0)->hidden.front().amplifier == 0);
  CHECK(effects.get(0)->hidden.front().is_infinite());

  // Four ticks of Speed III, then the fifth downgrades rather than expiring.
  const EffectTickContext ctx{20.0f, 20.0f, 0.0f};
  for (int i = 0; i < 4; ++i) {
    outcome = EffectOutcome{};
    effects.tick(0, ctx, outcome);
    CHECK(effects.get(0) != nullptr);
    CHECK(describe(outcome).empty());
  }
  outcome = EffectOutcome{};
  effects.tick(0, ctx, outcome);
  CHECK(effects.size() == 1);
  CHECK(effects.get(0)->amplifier == 0);
  CHECK(effects.get(0)->is_infinite());
  CHECK(describe(outcome) == " ~0");
  CHECK(effects.get(0)->hidden.empty());
}

void test_tick_behaviour() {
  // Regeneration heals one point on its own cadence, and only while health is
  // missing.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("regeneration", 200, 0), outcome);
    const EffectTickContext hurt{10.0f, 20.0f, 0.0f};
    outcome = EffectOutcome{};
    effects.tick(0, hurt, outcome); // duration 200, 200 % 50 == 0 -> fires
    CHECK(describe(outcome) == " e0@1000");
    outcome = EffectOutcome{};
    effects.tick(0, hurt, outcome); // 199 % 50 != 0
    CHECK(describe(outcome).empty());
    const EffectTickContext full{20.0f, 20.0f, 0.0f};
    for (int i = 0; i < 49; ++i) {
      outcome = EffectOutcome{};
      effects.tick(0, full, outcome);
    }
    // The 49th tick lands on duration 150, another multiple of 50 — so
    // Regeneration *did* fire, and a full health bar is why nothing came of it.
    CHECK(describe(outcome).empty());
  }

  // Poison is magic damage with a floor: it never finishes you off.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("poison", 25, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(outcome.events[0].kind == EffectEventKind::kDamage);
    CHECK(std::string(outcome.events[0].damage_type) == "magic");
    CHECK_NEAR(outcome.events[0].amount, 1.0f, 1e-6);

    ActiveEffects doomed;
    outcome = EffectOutcome{};
    doomed.add(make("poison", 25, 0), outcome);
    outcome = EffectOutcome{};
    doomed.tick(0, {1.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.empty()); // health is not above 1, so no damage
  }

  // Wither has no such floor: it is the effect that can kill.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("wither", 40, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {1.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(std::string(outcome.events[0].damage_type) == "wither");
  }

  // Hunger charges exhaustion every tick, scaled by the amplifier.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("hunger", 100, 2), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(outcome.events[0].kind == EffectEventKind::kExhaustion);
    CHECK_NEAR(outcome.events[0].amount, 0.015f, 1e-6); // 0.005 * (2 + 1)
  }

  // Saturation eats instantly: `eat(amplifier + 1, 1.0)`.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("saturation", 1, 1), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(outcome.events[0].kind == EffectEventKind::kEat);
    CHECK(outcome.events[0].food == 2);
    CHECK_NEAR(outcome.events[0].saturation, 1.0f, 1e-6);
    // A duration of 1 means one tick and gone.
    CHECK(describe(outcome) == " -22 e2@0");
    CHECK(effects.empty());
  }

  // Instant Health heals 4 << amplifier, once, then removes itself.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("instant_health", 1, 1), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {5.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(outcome.events[0].kind == EffectEventKind::kHeal);
    CHECK_NEAR(outcome.events[0].amount, 8.0f, 1e-6); // 4 << 1
    CHECK(outcome.removed.size() == 1);
    CHECK(effects.empty());
  }

  // Instant Damage is 6 << amplifier of magic damage.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("instant_damage", 1, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.events.size() == 1);
    CHECK(outcome.events[0].kind == EffectEventKind::kDamage);
    CHECK_NEAR(outcome.events[0].amount, 6.0f, 1e-6);
    CHECK(std::string(outcome.events[0].damage_type) == "magic");
  }

  // Absorption stays only while there are hearts to eat. That is a *liveness*
  // check on the tick, not on the landing.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("absorption", 600, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 4.0f}, outcome);
    CHECK(outcome.removed.empty() && effects.size() == 1);
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.removed.size() == 1);
    CHECK(effects.empty());
  }

  // The plain class ticks and does nothing at all.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("speed", 10, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(describe(outcome).empty());
    CHECK(effects.get(0)->duration == 9);
  }

  // LivingEntity.tickEffects re-sends the instance when its duration is a
  // multiple of 600 so a client that missed a packet recovers.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("speed", 601, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome); // 601 -> 600
    CHECK(describe(outcome) == " ~0");
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome); // 600 -> 599
    CHECK(describe(outcome).empty());
  }

  // An infinite effect is never decremented and never expires: it ticks against
  // the *entity's* counter, which is what makes an infinite Regeneration fire.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("regeneration", kInfiniteEffectDuration, 0), outcome);
    const EffectTickContext hurt{10.0f, 20.0f, 0.0f};
    outcome = EffectOutcome{};
    effects.tick(50, hurt, outcome); // 50 % 50 == 0
    CHECK(outcome.events.size() == 1 && outcome.removed.empty());
    outcome = EffectOutcome{};
    effects.tick(51, hurt, outcome);
    CHECK(outcome.events.empty());
    CHECK(effects.get(id_of("regeneration"))->is_infinite());
  }

  // An instance with no duration left is dropped before it can tick.
  {
    ActiveEffects effects;
    EffectOutcome outcome;
    effects.add(make("speed", 1, 0), outcome);
    outcome = EffectOutcome{};
    effects.tick(0, {20.0f, 20.0f, 0.0f}, outcome);
    CHECK(outcome.removed.size() == 1);
    CHECK(effects.empty());
  }
}

void test_attribute_modifiers() {
  ActiveEffects effects;
  EffectOutcome outcome;
  effects.add(make("speed", 600, 0), outcome);
  effects.add(make("strength", 600, 1), outcome);

  std::vector<EffectAttributeModifier> modifiers;
  effect_attribute_modifiers(effects, "minecraft:movement_speed", &modifiers);
  CHECK(modifiers.size() == 1);
  // MobEffect.AttributeTemplate.create's id is `minecraft:effect.<name>`, which
  // is what lets the client replace rather than stack the modifier.
  CHECK(modifiers[0].id == "minecraft:effect.speed");
  // The table's per-level amount is a float, so 0.2 arrives as 0.2f widened.
  CHECK_NEAR(modifiers[0].amount, 0.2f, 1e-6);
  CHECK(modifiers[0].operation == kModifierAddMultipliedTotal);

  // The amount is per level: Strength II is +6, not +3.
  modifiers.clear();
  effect_attribute_modifiers(effects, "minecraft:attack_damage", &modifiers);
  CHECK(modifiers.size() == 1);
  CHECK(modifiers[0].id == "minecraft:effect.strength");
  CHECK_NEAR(modifiers[0].amount, 6.0, 1e-9);
  CHECK(modifiers[0].operation == kModifierAddValue);
  CHECK_NEAR(effect_operation_total(effects, "minecraft:attack_damage", kModifierAddValue), 6.0,
             1e-9);

  // Nothing for an attribute no active effect names.
  modifiers.clear();
  effect_attribute_modifiers(effects, "minecraft:armor", &modifiers);
  CHECK(modifiers.empty());

  // AttributeInstance.calculateValue's order: the ADD_VALUE total lands on the
  // base, then each ADD_MULTIPLIED_TOTAL multiplies. A diamond sword (7.0) with
  // Strength II is 13.0 before the attack charge.
  CHECK_NEAR(apply_attribute_modifiers(7.0, {}), 7.0, 1e-9);
  {
    std::vector<EffectAttributeModifier> mixed;
    mixed.push_back(
        {"minecraft:effect.strength", "minecraft:attack_damage", 3.0, kModifierAddValue});
    mixed.push_back(
        {"minecraft:effect.weakness", "minecraft:attack_damage", -4.0, kModifierAddValue});
    CHECK_NEAR(apply_attribute_modifiers(7.0, mixed), 6.0, 1e-9);
  }
  {
    // ADD_MULTIPLIED_BASE scales the *post-ADD_VALUE* base, ADD_MULTIPLIED_TOTAL
    // the whole thing — which is why vanilla adds to `after_add`.
    std::vector<EffectAttributeModifier> mixed;
    mixed.push_back({"a", "x", 1.0, kModifierAddValue});
    mixed.push_back({"b", "x", 0.5, kModifierAddMultipliedBase});
    mixed.push_back({"c", "x", 1.0, kModifierAddMultipliedTotal});
    CHECK_NEAR(apply_attribute_modifiers(3.0, mixed), 12.0, 1e-9); // (3+1)*1.5*2
  }

  // Speed II on a 0.1 base is the +40% a client shows.
  CHECK_NEAR(apply_attribute_modifiers(0.1, {{"minecraft:effect.speed", "minecraft:movement_speed",
                                              0.4, kModifierAddMultipliedTotal}}),
             0.14, 1e-9);
}

void test_resistance_and_breathing() {
  ActiveEffects effects;
  EffectOutcome outcome;
  CHECK_NEAR(resistance_damage_multiplier(effects), 1.0f, 1e-6);

  // Resistance I is `(25 - 5) / 25` = 20% off; each level adds another 20%.
  effects.add(make("resistance", 600, 0), outcome);
  CHECK_NEAR(resistance_damage_multiplier(effects), 0.8f, 1e-6);
  effects.add(make("resistance", 600, 1), outcome);
  CHECK_NEAR(resistance_damage_multiplier(effects), 0.6f, 1e-6);
  // Resistance V would be negative, and vanilla floors it at zero.
  effects.add(make("resistance", 600, 5), outcome);
  CHECK_NEAR(resistance_damage_multiplier(effects), 0.0f, 1e-6);
  effects.remove_all();

  // MobEffectUtil.hasWaterBreathing: either effect, and nothing else.
  CHECK(!effects_grant_water_breathing(effects));
  effects.add(make("water_breathing", 600, 0), outcome);
  CHECK(effects_grant_water_breathing(effects));
  effects.remove_all();
  effects.add(make("conduit_power", 600, 0), outcome);
  CHECK(effects_grant_water_breathing(effects));
  effects.remove_all();
  effects.add(make("night_vision", 600, 0), outcome);
  CHECK(!effects_grant_water_breathing(effects));
}

void test_ordering_and_copy() {
  // The set is kept in id order so a sync and a test read the same list.
  ActiveEffects effects;
  EffectOutcome outcome;
  effects.add(make("wither", 100, 0), outcome);
  effects.add(make("speed", 100, 0), outcome);
  effects.add(make("absorption", 100, 0), outcome);
  CHECK(effects.size() == 3);
  CHECK(effects.all()[0].effect == 0);  // speed
  CHECK(effects.all()[1].effect == 19); // wither
  CHECK(effects.all()[2].effect == 21); // absorption

  // A copy carries the whole hidden chain: MobEffectInstance is a value type
  // even though vanilla's field is a self-reference.
  MobEffectInstance outer = make("speed", 5, 2);
  outer.hidden.push_back(make("speed", kInfiniteEffectDuration, 0));
  MobEffectInstance copy = outer;
  CHECK(copy.hidden.size() == 1);
  CHECK(copy.hidden.front().is_infinite());
  CHECK(copy.hidden.front().amplifier == 0);
}

void test_all_ambient() {
  // `LivingEntity.areAllEffectsAmbient`, the value DATA_EFFECT_AMBIENCE_ID
  // carries: true when *no visible, non-ambient* effect is in the set. The
  // hidden case is the one that makes it a value of its own rather than a
  // reading of the particle list — a set of hidden effects has an *empty* list
  // and still answers true, which is what `/effect give <player> speed 5 1 true`
  // produces. The ambient case is the half no code path in this server can
  // produce (no beacon, no conduit, every generated consume row is
  // `ambient = false`), so a hand-built instance is the only way to observe it.
  ActiveEffects effects;
  EffectOutcome outcome;

  // An empty set answers true (the loop never runs). Nothing *sends* that
  // answer — vanilla's empty branch writes the list alone, so a caller keeps the
  // last one a non-empty set gave — but the predicate itself is vanilla's.
  CHECK(effects_all_ambient(effects));

  // One visible ordinary effect is the only thing that makes it false.
  effects.add(make("speed", 100, 0), outcome);
  CHECK(!effects_all_ambient(effects));

  ActiveEffects hidden;
  MobEffectInstance hidden_speed = make("speed", 100, 0);
  hidden_speed.visible = false;
  hidden.add(hidden_speed, outcome);
  CHECK(effects_all_ambient(hidden));
  CHECK(effect_particles(hidden).empty());

  // ...and ambient is skipped the same way: an ambient effect's own particle is
  // drawn with alpha 38 rather than 255 (`MobEffect.createParticleOptions`), but
  // the byte and the list still have to agree with each other, and here both say
  // "skipped".
  ActiveEffects ambient;
  MobEffectInstance ambient_speed = make("speed", 100, 0);
  ambient_speed.ambient = true;
  ambient.add(ambient_speed, outcome);
  CHECK(effects_all_ambient(ambient));
  const std::vector<EffectParticle> ambient_swirl{EffectParticle{28, true, 0x2633EBFFu}};
  CHECK(effect_particles(ambient) == ambient_swirl);

  // A visible non-ambient effect anywhere in the set decides, even beside an
  // ambient one.
  ambient.add(make("glowing", 100, 0), outcome);
  CHECK(ambient.size() == 2);
  CHECK(!effects_all_ambient(ambient));
  ambient.remove(mob_effect_id_of(*mob_effect_by_name("glowing")));
  CHECK(effects_all_ambient(ambient));

  // Hidden and ambient together are still "all ambient": neither one is the
  // entry that returns false.
  ActiveEffects both;
  MobEffectInstance hidden_glow = make("glowing", 100, 0);
  hidden_glow.visible = false;
  both.add(hidden_glow, outcome);
  MobEffectInstance ambient_speed2 = make("speed", 100, 0);
  ambient_speed2.ambient = true;
  both.add(ambient_speed2, outcome);
  CHECK(both.size() == 2);
  CHECK(effects_all_ambient(both));
}

} // namespace

int main() {
  test_registry();
  test_glowing_bit();
  test_invisibility_bit();
  test_tick_predicate();
  test_add_and_merge();
  test_hidden_effect_chain();
  test_tick_behaviour();
  test_attribute_modifiers();
  test_resistance_and_breathing();
  test_ordering_and_copy();
  test_all_ambient();
  if (failures != 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("effect tests passed\n");
  return 0;
}
