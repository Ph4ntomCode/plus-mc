// SPDX-License-Identifier: GPL-3.0-or-later
//
// Status effects: the minecraft:mob_effect registry as the engine reads it,
// the MobEffectInstance value type, the merge/expiry rules LivingEntity and
// MobEffectInstance implement, the per-tick behaviour of each effect class and
// the attribute modifiers an effect contributes (Phase 3 milestone G).
//
// The table itself is generated (world/effect_table.h) from the data
// generator's registries.json plus the decompiled MobEffects.java, because an
// effect is a *built-in* registry and its colour, category and attribute
// modifiers are constructor arguments rather than data. The table index is the
// protocol id, which is what an update_mob_effect packet carries — so
// `effect` fields here are wire ids, "minecraft:speed" is index 0, and
// `mob_effect_by_id` is an array index.
//
// What is modelled, and what is deliberately not:
//   * the one container vanilla puts an effect in: `ActiveEffects` is
//     `LivingEntity.activeEffects`, keyed by effect;
//   * `MobEffectInstance.update`'s merge rules, *including* the hidden-effect
//     chain (a short strong instance under a long weak one);
//   * every effect class's `shouldApplyEffectTickThisTick` / `applyEffectTick`
//     pair — regeneration, poison, wither, hunger, saturation, instant
//     health/damage and absorption;
//   * each effect's attribute modifiers, which is how Speed, Strength, Haste
//     and Health Boost reach the engine;
//   * the `MobEffectUtil.hasWaterBreathing` and Resistance gates the survival
//     layer reads.
//
// Out of scope, and why: the effects with no per-tick behaviour this server can
// *do* (Bad Omen and Raid Omen need raid spawning, Oozing/Weaving/Infested need
// to spawn entities, Levitation/Nausea/Blindness are client-side) tick as
// no-ops, which is what `kEffectPlain` means.
//
// A mob holds the same `ActiveEffects` and ticks it on its own clock
// (`mob_add_effect` / `mob_tick_effects` in world/mob.h), because vanilla puts
// an effect on any LivingEntity and a spectral arrow's 200 ticks of Glowing land
// on one. What a mob does *not* yet do with them is the two things that would
// have to leave this module: an effect *event* (a mob's poison or regeneration
// would have to travel world/mob.h's own `hurt_mob`, not a player's survival
// sink) and attribute modifiers (a mob's Speed or Strength is not read by the
// AI). The set is kept whole so the effects that raise a shared flag — the
// glowing bit today — are exact.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/effect_table.h"

namespace mc::world {

// --- the registry -----------------------------------------------------------

// nullptr for an unknown or malformed name ("speed" is normalized first).
const GeneratedMobEffect* mob_effect_by_name(std::string_view name);
// nullptr when the id is out of range.
const GeneratedMobEffect* mob_effect_by_id(int32_t id);
// Registry index (== the wire id), or -1.
int32_t mob_effect_id_of(const GeneratedMobEffect& effect);

// `MobEffects.GLOWING`'s registry id, resolved on the first call. -1 only if the
// table has no such effect, which the generated rows make unreachable. One
// lookup for every entity that can hold it: the bit
// `LivingEntity.updateGlowingStatus` writes into the shared flags is
// `hasEffect(MobEffects.GLOWING)` whether the entity is a mob or a player.
int32_t glowing_effect_id();

// --- MobEffectInstance ------------------------------------------------------

// MobEffectInstance.INFINITE_DURATION / MIN_AMPLIFIER / MAX_AMPLIFIER.
constexpr int kInfiniteEffectDuration = -1;
constexpr int kMaxEffectAmplifier = 255;

struct MobEffectInstance {
  int32_t effect = 0; // the mob_effect id (its table index)
  int duration = 0;   // ticks; kInfiniteEffectDuration for "infinite"
  int amplifier = 0;
  bool ambient = false;
  bool visible = true;
  bool show_icon = true;
  // MobEffectInstance.hiddenEffect: the instance this one reverts to when its
  // duration runs out. `/effect give @s speed 5 3` on top of an infinite
  // Speed I leaves a 100-tick Speed IV hiding an infinite Speed I.
  //
  // Vanilla's is a `@Nullable MobEffectInstance` — a self-reference — which a
  // value type cannot hold directly. It is a list that never has more than one
  // element, which keeps the field copyable and the chain exactly as deep as
  // vanilla's (`hidden.front().hidden` is the next one down).
  std::vector<MobEffectInstance> hidden;

  const GeneratedMobEffect* def() const {
    return mob_effect_by_id(effect);
  }
  bool is_infinite() const {
    return duration == kInfiniteEffectDuration;
  }
  bool has_remaining_duration() const {
    return is_infinite() || duration > 0;
  }
  // MobEffectInstance.update. Returns whether anything changed, which is
  // LivingEntity.addEffect's return value (and so whether the client needs a
  // packet).
  bool update(const MobEffectInstance& take_over);
};

// Whether two instances are the same effect in the same state. Vanilla's
// MobEffectInstance overrides no equals, so a component that holds instances
// compares them by identity — which for two instances that differ in no field
// is the wrong answer twice over (a potion of healing is not a water bottle).
// ItemStack.isSameItemSameComponents uses this for a stack's custom potion
// effects; the hidden chain is part of the state, so it is compared too.
bool same_effect_instance(const MobEffectInstance& a, const MobEffectInstance& b);

// --- what an effect asks for ------------------------------------------------

// One thing an effect asked for. The module never touches health for *damage*
// itself: a tick's damage has to travel the full damage pipeline (armour,
// enchantment protection, absorption, death), so it leaves as an event and the
// session routes it through the same sink as every other source. Healing is a
// direct number because LivingEntity.heal is not part of that pipeline.
enum class EffectEventKind : uint8_t {
  kHeal,       // LivingEntity.heal(amount)
  kDamage,     // a damage source of `damage_type` for `amount`
  kEat,        // FoodData.eat(food, saturation)
  kExhaustion, // Player.causeFoodExhaustion(amount)
  kAbsorb,     // setAbsorptionAmount(max(current, amount))
};

struct EffectEvent {
  EffectEventKind kind = EffectEventKind::kHeal;
  int32_t effect = 0;
  float amount = 0.0f;               // heal / damage / exhaustion / absorb
  int food = 0;                      // kEat
  float saturation = 0.0f;           // kEat
  const char* damage_type = nullptr; // kDamage
  // The *causing* entity a kDamage names, and its display name. Vanilla's
  // potion harm is `damageSources().indirectMagic(source, owner)` — the thrower
  // for a thrown potion, the drinker themself for a drunk one — so the attacker
  // is not always "whoever the effect is on", and the client's hit feedback and
  // the death message both read it. -1 and empty for a source with no attacker.
  int32_t causing_entity_id = -1;
  std::string causing_name;
};

// What a tick or an `add` produced, so the session only has to act on what
// actually happened.
struct EffectOutcome {
  // Numbers to apply, in the order the effects produced them.
  std::vector<EffectEvent> events;
  // Effects that landed for the first time: ServerPlayer.onEffectAdded, i.e.
  // update_mob_effect with the blend flag set.
  std::vector<int32_t> added;
  // Effects whose instance changed (a merge, a hidden-effect downgrade, or
  // vanilla's own 600-tick refresh): onEffectUpdated, blend clear.
  std::vector<int32_t> updated;
  // Effects that expired or removed themselves: onEffectsRemoved, i.e.
  // remove_mob_effect.
  std::vector<int32_t> removed;
};

// The entity numbers an effect's tick reads.
struct EffectTickContext {
  float health = 0.0f;
  float max_health = 0.0f;
  // LivingEntity.getAbsorptionAmount; Absorption's tick returns false — which
  // removes the effect — once it is gone.
  float absorption = 0.0f;
};

// LivingEntity.activeEffects. Kept ordered by effect id so a join sync and a
// test both read the same list every time; vanilla's is a map keyed by holder
// and has no order of its own.
class ActiveEffects {
public:
  bool empty() const {
    return effects_.empty();
  }
  int size() const {
    return static_cast<int>(effects_.size());
  }
  bool has(int32_t effect) const {
    return index_of(effect) >= 0;
  }
  const MobEffectInstance* get(int32_t effect) const;
  const std::vector<MobEffectInstance>& all() const {
    return effects_;
  }
  void clear() {
    effects_.clear();
  }

  // LivingEntity.addEffect (minus its canBeAffected gate: nothing here is
  // immune). `out` collects the onEffectStarted side effects — Absorption's
  // setAbsorptionAmount — and the effect id, either as newly added or as
  // changed.
  bool add(const MobEffectInstance& instance, EffectOutcome& out);
  // LivingEntity.removeEffect / removeAllEffects.
  bool remove(int32_t effect);
  bool remove_all();

  // LivingEntity.tickEffects. Ticks every instance, removes the ones that
  // expired or whose applyEffectTick returned false, and reports both them and
  // the instances that changed. `entity_tick_count` is `Entity.tickCount`,
  // which is the tick counter an *infinite* effect uses.
  void tick(int entity_tick_count, const EffectTickContext& ctx, EffectOutcome& out);

private:
  int index_of(int32_t effect) const;
  std::vector<MobEffectInstance> effects_;
};

// `LivingEntity.hasEffect(MobEffects.GLOWING)` over any entity's effect set —
// the answer the shared flags' glowing bit (flag 6) carries. It is a derivation
// rather than a stored bit so that a set which ticks its own durations (a mob's
// and a player's both do) cannot disagree with the flag the client is drawn from.
bool effects_glow(const ActiveEffects& effects);

// `LivingEntity.updateInvisibilityStatus`'s answer over the same set:
// `hasEffect(MobEffects.INVISIBILITY)`, the shared flags' invisibility bit
// (flag 5). Its empty-set branch (`removeEffectParticles(); setInvisible(false);
// return;`) is what `has` says over an empty set anyway, so the set stays the
// only truth here too: `/effect clear`, an expiry and a death all take the bit
// down by taking the instance out, with nothing to remember. The id is resolved
// inside effect.cpp — unlike Glowing's, nothing has to *write* an invisibility
// instance by hand, so the lookup is not part of this header.
bool effects_invisible(const ActiveEffects& effects);

// --- the particle list every client draws (DATA_EFFECT_PARTICLES) -----------

// One entry of `LivingEntity.DATA_EFFECT_PARTICLES`: the particle a client draws
// an effect's swirl with. `particle_type` is a minecraft:particle_type registry
// id; `has_color` is whether that type's own stream codec carries the u32
// colour, which only ColorParticleOption's does.
struct EffectParticle {
  uint32_t particle_type = 0;
  bool has_color = false;
  uint32_t color = 0;
  bool operator==(const EffectParticle&) const = default;
};

// `LivingEntity.updateSynchronizedMobEffectParticles` over a set —
// `MobEffectInstance.getParticleOptions()` for every *visible* instance, in the
// set's own (id-ordered) order:
//
//   activeEffects.values().stream().filter(MobEffectInstance::isVisible)
//                              .map(MobEffectInstance::getParticleOptions)
//
// That factory is `MobEffect.createParticleOptions`, and every effect in the
// registry but two is built with MobEffect's two-argument constructor, whose
// factory tints minecraft:entity_effect with `ARGB.color(ambient ? 38 : 255,
// color)` — the effect's own colour and `AMBIENT_ALPHA = Mth.floor(38.25f)`.
// The two exceptions are the omens, built with an explicit ParticleOptions: a
// bare SimpleParticleType whose codec carries no colour at all, and one that is
// not the effect's own colour (minecraft:raid_omen, minecraft:trial_omen).
//
// The list is only one of the *two* values that method writes: it writes
// `DATA_EFFECT_AMBIENCE_ID` (index 11) beside it, which is
// effects_all_ambient's — see just below.
std::vector<EffectParticle> effect_particles(const ActiveEffects& effects);

// --- the ambience byte beside that list (DATA_EFFECT_AMBIENCE_ID) -----------

// `LivingEntity.areAllEffectsAmbient`, the value `DATA_EFFECT_AMBIENCE_ID`
// (index 11, a boolean) carries: *no visible, non-ambient effect in the set*.
// Spelled the way vanilla spells it, because it is not the same question as
// "every effect is ambient" — both hidden halves skip the entry that decides:
//
//   for (MobEffectInstance effect : effects) {
//     if (!effect.isVisible() || effect.isAmbient()) continue;
//     return false;
//   }
//   return true;
//
// An all-invisible set therefore answers true as well, and that is the half this
// server can reach: an effect can be made invisible here (`/effect give ...
// true`), while no code path creates an *ambient* instance (no beacon, no
// conduit, and every generated consume row is `ambient = false`). An ambient
// instance would take exactly this path — the same predicate over the same set.
//
// The reader is a client: `LivingEntity.tick`'s client branch multiplies the
// divisor it draws a swirl with by 5 when this is true, so the swirls of a set
// that answers true appear five times as sparsely. It is a value of its own
// rather than something derivable from the particle list above, precisely
// because a set of hidden effects has an empty list and still answers true.
//
// One thing this predicate deliberately does *not* do is answer for an empty
// set: it says true (the loop runs zero times), but vanilla never writes that
// answer — `updateInvisibilityStatus`'s empty branch calls
// `removeEffectParticles`, which sets the *list* alone, so the byte keeps the
// last answer a non-empty set gave. Keeping that stale is the caller's job (see
// `publish_player_effects`), because it is about what has been sent rather than
// about what the set is.
bool effects_all_ambient(const ActiveEffects& effects);

// --- tick behaviour ---------------------------------------------------------

// MobEffect.applyInstantaneousEffect: what an instantaneous effect does when it
// is *applied* rather than ticked. This is the path a drunk potion takes —
// PotionContents.applyToLivingEntity checks isInstantaneous and calls this
// instead of addEffect — while a duration-1 instance from `/effect give` ticks
// through apply_effect_tick below. Only HealOrHarmMobEffect overrides it, so
// the numbers are that class's own at vanilla's scale of 1.0: `4 << amplifier`
// health and `6 << amplifier` damage, the same as its tick.
//
// `harm_damage_type` is the caller's because the two paths really do differ:
// a ticked instant effect hurts with `mob.damageSources().magic()`, while a
// potion's instantaneous effect is applied with the drinker as both source and
// owner (`damageSources().indirectMagic(source, owner)`), i.e. indirect_magic.
// A name apply_damage does not know yields a source it refuses, so a typo
// cannot turn into free damage.
//
// HealOrHarmMobEffect.applyInstantaneousEffect's own arithmetic —
// `(int)(scale * ((harm ? 6 : 4) << amplifier) + 0.5)`, with the same saturated
// shift the tick path uses — exposed because a *thrown* potion has to resolve
// `harm` against its victim before it computes the number: vanilla's test is
// `isHarm == mob.isInvertedHealAndHarm()`, so instant health damages a zombie
// (and for 6 a level rather than 4). `scale` is 1.0 for a drink, the splash
// falloff `1 - sqrt(distance) / 4` for a thrown splash potion, and 0.5 for a
// cloud.
float instant_effect_amount(bool harm, int amplifier, double scale);

// A name apply_damage does not know yields a source it refuses, so a typo
// cannot turn into free damage.
//
// `scale` is `applyInstantaneousEffect`'s last argument: 1.0 for a drunk potion
// and for `/effect give`, the splash falloff `1 - sqrt(distance) / 4` when a
// thrown splash potion reaches the entity, and the fixed 0.5 a lingering cloud
// uses. It multiplies the amount *before* the `+ 0.5`, which is why it can
// round to zero at the edge of a splash.
void apply_instant_effect(const GeneratedMobEffect& def, int amplifier,
                          const char* harm_damage_type, EffectOutcome& out, double scale = 1.0);

// MobEffect.shouldApplyEffectTickThisTick for the classes this server models.
// `tick_count` is the instance's duration (or the entity's own tick counter for
// an infinite one), which is why Regeneration I fires on every 50th tick of the
// effect while a duration-1 Instant Health fires immediately.
bool effect_should_apply_tick(uint8_t kind, int tick_count, int amplifier);

// --- attributes -------------------------------------------------------------

// One effect-contributed attribute modifier, with the amplifier already folded
// in (MobEffect.AttributeTemplate.create: amount * (amplifier + 1)).
struct EffectAttributeModifier {
  std::string id;        // "minecraft:effect.speed"
  const char* attribute; // minecraft:movement_speed
  double amount = 0.0;
  uint8_t operation = 0; // kModifier*
};

// The modifiers the active effects contribute to `attribute`, in vanilla's own
// id form so the client replaces rather than stacks them.
void effect_attribute_modifiers(const ActiveEffects& effects, std::string_view attribute,
                                std::vector<EffectAttributeModifier>* out);

// The summed amount the active effects contribute to `attribute` under one
// operation, which is what AttributeInstance.calculateValue reads.
double effect_operation_total(const ActiveEffects& effects, std::string_view attribute,
                              uint8_t operation);

// AttributeInstance.calculateValue: sum ADD_VALUE onto the base, add the base
// scaled by ADD_MULTIPLIED_BASE, then multiply by (1 + each
// ADD_MULTIPLIED_TOTAL).
double apply_attribute_modifiers(double base,
                                 const std::vector<EffectAttributeModifier>& modifiers);

// MobEffectUtil.hasWaterBreathing: water_breathing or conduit_power.
bool effects_grant_water_breathing(const ActiveEffects& effects);

// Resistance's damage multiplier, (25 - 5 * (amplifier + 1)) / 25 — 0.8 at
// Resistance I. Returns 1.0 with no Resistance effect; the caller checks the
// BYPASSES_RESISTANCE tag itself, because that is damage.h's job.
float resistance_damage_multiplier(const ActiveEffects& effects);

} // namespace mc::world
