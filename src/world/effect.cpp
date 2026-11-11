// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/effect.h"

#include <algorithm>
#include <cmath>

#include "world/item.h"

namespace mc::world {

namespace {

// The interval an effect's class ticks on, or 0 for a class that has no
// interval (it either fires every tick or never does).
int class_interval(uint8_t kind, int amplifier) {
  // Java's `50 >> amplifier` masks the shift count; clamping keeps the same
  // answer without relying on the platform's shift behaviour.
  const int shift = std::min(amplifier, 31);
  switch (kind) {
  case kEffectRegeneration:
    return 50 >> shift;
  case kEffectPoison:
    return 25 >> shift;
  case kEffectWither:
    return 40 >> shift;
  default:
    return 0;
  }
}

} // namespace

void apply_instant_effect(const GeneratedMobEffect& def, int amplifier,
                          const char* harm_damage_type, EffectOutcome& out, double scale) {
  // HealOrHarmMobEffect.applyInstantaneousEffect at the caller's scale (1.0 for
  // the tick path). The `isHarm == mob.isInvertedHealAndHarm()` test is against
  // a player's `false`, so for a player the flag alone decides which way it goes
  // — see instant_effect_amount and the thrown-potion path, which resolves it
  // against its victim instead.
  if (!def.instantaneous)
    return;
  EffectEvent event;
  event.effect = mob_effect_id_of(def);
  event.amount = instant_effect_amount(def.harm, amplifier, scale);
  if (def.harm) {
    event.kind = EffectEventKind::kDamage;
    event.damage_type = harm_damage_type;
  } else {
    event.kind = EffectEventKind::kHeal;
  }
  out.events.push_back(event);
}

float instant_effect_amount(bool harm, int amplifier, double scale) {
  // `(int)(scale * (4 << amplification) + 0.5)`, saturated the same way the tick
  // path saturates it (see scaled_amount): `1 << min(amplifier, 20)` doubled for
  // the harm class's 6.
  const int shift = std::min(amplifier, 20);
  const double magnitude = static_cast<double>((harm ? 6 : 4) << shift);
  return static_cast<float>(std::floor(scale * magnitude + 0.5));
}

namespace {

// MobEffect.applyEffectTick, per class, for the player it is on
// (`isInvertedHealAndHarm` is false for a player, which is the case here).
// Returns false when the effect removes itself — only Absorption does.
bool apply_effect_tick(const GeneratedMobEffect& def, int amplifier, const EffectTickContext& ctx,
                       EffectOutcome& out) {
  EffectEvent event;
  event.effect = mob_effect_id_of(def);
  switch (def.kind) {
  case kEffectRegeneration:
    // RegenerationMobEffect: only while health is missing.
    if (ctx.health < ctx.max_health) {
      event.kind = EffectEventKind::kHeal;
      event.amount = 1.0f;
      out.events.push_back(event);
    }
    return true;
  case kEffectPoison:
    // PoisonMobEffect: magic damage, and it never finishes you off
    // (`getHealth() > 1`), which is what makes poison survivable.
    if (ctx.health > 1.0f) {
      event.kind = EffectEventKind::kDamage;
      event.amount = 1.0f;
      event.damage_type = "magic";
      out.events.push_back(event);
    }
    return true;
  case kEffectWither:
    // WitherMobEffect has no health gate: it can kill.
    event.kind = EffectEventKind::kDamage;
    event.amount = 1.0f;
    event.damage_type = "wither";
    out.events.push_back(event);
    return true;
  case kEffectHunger:
    event.kind = EffectEventKind::kExhaustion;
    event.amount = 0.005f * static_cast<float>(amplifier + 1);
    out.events.push_back(event);
    return true;
  case kEffectSaturation:
    event.kind = EffectEventKind::kEat;
    event.food = amplifier + 1;
    event.saturation = 1.0f;
    out.events.push_back(event);
    return true;
  case kEffectInstantHealthOrHarm:
    // An InstantaneousMobEffect reached through its *tick*, which is how an
    // `/effect give` of one applies: the same numbers as the potion path, but
    // its harm is `damageSources().magic()` rather than indirect_magic.
    apply_instant_effect(def, amplifier, "magic", out);
    return true;
  case kEffectAbsorption:
    // AbsorptionMobEffect.applyEffectTick is a *liveness* check: the effect
    // stays only while there are absorption hearts to eat.
    return ctx.absorption > 0.0f;
  default:
    // MobEffect.applyEffectTick returns true and does nothing.
    return true;
  }
}

// MobEffectInstance.setDetailsFrom: the effect stays, everything else moves.
void set_details_from(MobEffectInstance& instance, const MobEffectInstance& from) {
  instance.duration = from.duration;
  instance.amplifier = from.amplifier;
  instance.ambient = from.ambient;
  instance.visible = from.visible;
  instance.show_icon = from.show_icon;
}

// MobEffectInstance.tickDownDuration, whose every write goes through
// `mapDuration`. That guard is two-sided and both sides are load-bearing here:
// an *infinite* duration is not a number to decrement, and a duration already
// at 0 is left alone. The second matters more than it looks, because -1 is the
// infinite sentinel — decrementing a 0 would silently turn an expired instance
// into a permanent one.
//
// The hidden chain is ticked first, so the layer underneath is one tick nearer
// being reachable than the layer above it.
void tick_down_duration(MobEffectInstance& instance) {
  for (MobEffectInstance& below : instance.hidden)
    tick_down_duration(below);
  if (instance.is_infinite() || instance.duration == 0)
    return;
  --instance.duration;
}

// MobEffectInstance.downgradeToHiddenEffect: the layer above expired, so what
// was hiding under it becomes the live instance and the rest of the chain moves
// up with it.
bool downgrade_to_hidden(MobEffectInstance& instance) {
  if (instance.duration != 0 || instance.hidden.empty())
    return false;
  MobEffectInstance below = std::move(instance.hidden.front());
  instance.hidden.clear();
  set_details_from(instance, below);
  instance.hidden = std::move(below.hidden);
  return true;
}

// MobEffectInstance.tickServer, including the 600-tick refresh
// (`effect.getDuration() % 600 != 0` is what skips it) and the `reports_removed`
// contract of `LivingEntity.tickEffects`.
bool tick_instance(MobEffectInstance& instance, int entity_tick_count, const EffectTickContext& ctx,
                   EffectOutcome& out) {
  if (!instance.has_remaining_duration())
    return false;
  const GeneratedMobEffect* def = instance.def();
  if (!def)
    return false; // an id this build does not know: drop it rather than tick it
  const int tick_count = instance.is_infinite() ? entity_tick_count : instance.duration;
  if (effect_should_apply_tick(def->kind, tick_count, instance.amplifier) &&
      !apply_effect_tick(*def, instance.amplifier, ctx, out))
    return false;
  tick_down_duration(instance);
  if (downgrade_to_hidden(instance))
    out.updated.push_back(instance.effect);
  // Vanilla's loop `continue`s here when tickServer reported the effect gone,
  // so an instance that just expired is removed and never also refreshed.
  if (!instance.has_remaining_duration())
    return false;
  // LivingEntity.tickEffects re-sends the instance every 30 seconds so a client
  // that missed a packet recovers. An infinite duration never matches: -1 % 600
  // is -1 in both Java and C++.
  if (instance.duration % 600 == 0)
    out.updated.push_back(instance.effect);
  return true;
}

} // namespace

const GeneratedMobEffect* mob_effect_by_name(std::string_view name) {
  const std::string normalized = normalize_item_name(name);
  if (normalized.empty())
    return nullptr;
  for (int i = 0; i < kMobEffectCount; ++i)
    if (normalized == kMobEffects[i].name)
      return &kMobEffects[i];
  return nullptr;
}

const GeneratedMobEffect* mob_effect_by_id(int32_t id) {
  if (id < 0 || id >= kMobEffectCount)
    return nullptr;
  return &kMobEffects[id];
}

int32_t mob_effect_id_of(const GeneratedMobEffect& effect) {
  const auto index = &effect - kMobEffects;
  if (index < 0 || index >= kMobEffectCount)
    return -1;
  return static_cast<int32_t>(index);
}

int32_t glowing_effect_id() {
  // One lookup, the first time anything asks: the table is generated, so the id
  // is its row index and never changes while the process runs.
  static const int32_t id = [] {
    const GeneratedMobEffect* effect = mob_effect_by_name("minecraft:glowing");
    return effect == nullptr ? -1 : mob_effect_id_of(*effect);
  }();
  return id;
}

bool effects_glow(const ActiveEffects& effects) {
  const int32_t id = glowing_effect_id();
  return id >= 0 && effects.has(id);
}

namespace {
// `MobEffects.INVISIBILITY`'s registry id, resolved on the first call: the table
// is generated, so the id is its row index and never changes while the process
// runs. File-local because no caller writes an invisibility instance by hand the
// way the spectral arrow writes a Glowing one.
int32_t invisibility_effect_id() {
  static const int32_t id = [] {
    const GeneratedMobEffect* effect = mob_effect_by_name("minecraft:invisibility");
    return effect == nullptr ? -1 : mob_effect_id_of(*effect);
  }();
  return id;
}
} // namespace

bool effects_invisible(const ActiveEffects& effects) {
  const int32_t id = invisibility_effect_id();
  return id >= 0 && effects.has(id);
}

std::vector<EffectParticle> effect_particles(const ActiveEffects& effects) {
  // The three particle options any effect's factory names: minecraft:entity_effect
  // (the tintable one) and the two the omens are built with, whose codecs carry
  // no colour at all.
  constexpr uint32_t kEntityEffectParticle = 28;
  constexpr uint32_t kRaidOmenParticle = 120;
  constexpr uint32_t kTrialOmenParticle = 121;
  // MobEffect.AMBIENT_ALPHA = Mth.floor(38.25f).
  constexpr uint32_t kAmbientAlpha = 38;
  std::vector<EffectParticle> out;
  for (const MobEffectInstance& instance : effects.all()) {
    // `filter(MobEffectInstance::isVisible)`: an effect given with
    // `hideParticles` (or one that is not visible for any other reason) draws
    // nothing and is not in the list at all.
    if (!instance.visible)
      continue;
    const GeneratedMobEffect* def = instance.def();
    if (!def)
      continue;
    // The registry's only two explicit-particle constructors (`RaidOmenMobEffect`
    // and trial_omen's own MobEffect): the particle is a constant that is not the
    // effect's colour.
    const std::string_view name(def->name);
    if (name == "minecraft:raid_omen") {
      out.push_back(EffectParticle{kRaidOmenParticle, false, 0});
      continue;
    }
    if (name == "minecraft:trial_omen") {
      out.push_back(EffectParticle{kTrialOmenParticle, false, 0});
      continue;
    }
    // MobEffect's own factory: ARGB.color(alpha, color), i.e. the alpha byte
    // over the effect's 24-bit colour.
    const uint32_t alpha = instance.ambient ? kAmbientAlpha : 255u;
    out.push_back(
        EffectParticle{kEntityEffectParticle, true, (alpha << 24) | (def->color & 0xFFFFFFu)});
  }
  return out;
}

bool effects_all_ambient(const ActiveEffects& effects) {
  // The first *visible, non-ambient* instance decides; a hidden one and an
  // ambient one are both skipped, so a set of nothing but those answers true
  // (the empty set among them — `areAllEffectsAmbient`'s own answer, which is
  // only ever written while the set is non-empty; see the header).
  for (const MobEffectInstance& instance : effects.all()) {
    if (!instance.visible || instance.ambient)
      continue;
    return false;
  }
  return true;
}

// MobEffectInstance.isShorterDurationThan. Note it is *directional*, and
// MobEffectInstance.update asks it in both directions: `takeOver.isShorter(this)`
// when the new instance is stronger, and `this.isShorter(takeOver)` when it is
// not. Getting the direction wrong costs the hidden-effect chain.
namespace {
bool is_shorter_than(const MobEffectInstance& a, const MobEffectInstance& b) {
  return !a.is_infinite() && (a.duration < b.duration || b.is_infinite());
}
} // namespace

bool same_effect_instance(const MobEffectInstance& a, const MobEffectInstance& b) {
  if (a.effect != b.effect || a.duration != b.duration || a.amplifier != b.amplifier ||
      a.ambient != b.ambient || a.visible != b.visible || a.show_icon != b.show_icon)
    return false;
  if (a.hidden.size() != b.hidden.size())
    return false;
  for (size_t i = 0; i < a.hidden.size(); ++i)
    if (!same_effect_instance(a.hidden[i], b.hidden[i]))
      return false;
  return true;
}

bool MobEffectInstance::update(const MobEffectInstance& take_over) {
  // MobEffectInstance.update, including the hidden-effect chain: a shorter
  // stronger instance is remembered underneath the longer one rather than
  // thrown away, so what is left when the strong one expires is the old effect.
  bool changed = false;
  if (take_over.amplifier > amplifier) {
    if (is_shorter_than(take_over, *this)) {
      // `this.hiddenEffect = new MobEffectInstance(this);
      //  this.hiddenEffect.hiddenEffect = prevHiddenEffect;` — a copy of the
      // *current* details (not of the hidden chain; the copy constructor is
      // setDetailsFrom) with the old chain hanging under it.
      std::vector<MobEffectInstance> prev = std::move(hidden);
      hidden.clear();
      MobEffectInstance layer;
      layer.effect = effect;
      set_details_from(layer, *this);
      layer.hidden = std::move(prev);
      hidden.push_back(std::move(layer));
    }
    amplifier = take_over.amplifier;
    duration = take_over.duration;
    changed = true;
  } else if (is_shorter_than(*this, take_over)) {
    if (take_over.amplifier == amplifier) {
      duration = take_over.duration;
      changed = true;
    } else if (hidden.empty()) {
      hidden.push_back(take_over);
    } else {
      hidden.front().update(take_over);
    }
  }
  // Note the asymmetry vanilla has: `ambient` is taken over only when the new
  // instance is not ambient or this one already was, whereas visible and
  // showIcon are taken over whenever they differ.
  if ((!take_over.ambient && ambient) || changed) {
    ambient = take_over.ambient;
    changed = true;
  }
  if (take_over.visible != visible) {
    visible = take_over.visible;
    changed = true;
  }
  if (take_over.show_icon != show_icon) {
    show_icon = take_over.show_icon;
    changed = true;
  }
  return changed;
}

int ActiveEffects::index_of(int32_t effect) const {
  for (size_t i = 0; i < effects_.size(); ++i)
    if (effects_[i].effect == effect)
      return static_cast<int>(i);
  return -1;
}

const MobEffectInstance* ActiveEffects::get(int32_t effect) const {
  const int index = index_of(effect);
  return index < 0 ? nullptr : &effects_[static_cast<size_t>(index)];
}

bool ActiveEffects::add(const MobEffectInstance& instance, EffectOutcome& out) {
  const GeneratedMobEffect* def = instance.def();
  if (!def)
    return false;
  // MobEffectCategory is not what decides whether an effect lands; what does is
  // `MobEffectInstance.update` plus `onEffectStarted`. The one immediate side
  // effect this server can act on is Absorption's.
  const int index = index_of(instance.effect);
  bool changed = false;
  if (index < 0) {
    effects_.push_back(instance);
    // Keeping the order stable (rather than vanilla's map-by-holder) makes the
    // packet order and the tests deterministic.
    std::sort(
        effects_.begin(), effects_.end(),
        [](const MobEffectInstance& a, const MobEffectInstance& b) { return a.effect < b.effect; });
    changed = true;
    out.added.push_back(instance.effect);
  } else if (effects_[static_cast<size_t>(index)].update(instance)) {
    out.updated.push_back(instance.effect);
    changed = true;
  }
  if (def->kind == kEffectAbsorption) {
    EffectEvent event;
    event.kind = EffectEventKind::kAbsorb;
    event.effect = instance.effect;
    // AbsorptionMobEffect.onEffectStarted: max(current, 4 * (1 + amplifier)).
    event.amount = 4.0f * static_cast<float>(instance.amplifier + 1);
    out.events.push_back(event);
  }
  return changed;
}

bool ActiveEffects::remove(int32_t effect) {
  const int index = index_of(effect);
  if (index < 0)
    return false;
  effects_.erase(effects_.begin() + index);
  return true;
}

bool ActiveEffects::remove_all() {
  if (effects_.empty())
    return false;
  effects_.clear();
  return true;
}

void ActiveEffects::tick(int entity_tick_count, const EffectTickContext& ctx, EffectOutcome& out) {
  for (size_t i = 0; i < effects_.size();) {
    MobEffectInstance& instance = effects_[i];
    if (tick_instance(instance, entity_tick_count, ctx, out)) {
      ++i;
      continue;
    }
    out.removed.push_back(instance.effect);
    effects_.erase(effects_.begin() + static_cast<long>(i));
  }
}

bool effect_should_apply_tick(uint8_t kind, int tick_count, int amplifier) {
  const int interval = class_interval(kind, amplifier);
  if (interval > 0)
    return tick_count % interval == 0;
  switch (kind) {
  case kEffectRegeneration:
  case kEffectPoison:
  case kEffectWither:
    // The interval collapsed to zero, which vanilla treats as "every tick".
    return true;
  case kEffectHunger:
  case kEffectAbsorption:
    return true;
  case kEffectInstantHealthOrHarm:
  case kEffectSaturation:
    // InstantaneousMobEffect: `remainingDuration >= 1`, so it fires on the
    // first tick and then expires.
    return tick_count >= 1;
  default:
    // MobEffect.shouldApplyEffectTickThisTick is `false` for the plain class
    // (and for the six spawn-flavoured subclasses this server does not model).
    return false;
  }
}

void effect_attribute_modifiers(const ActiveEffects& effects, std::string_view attribute,
                                std::vector<EffectAttributeModifier>* out) {
  if (!out)
    return;
  for (const MobEffectInstance& instance : effects.all()) {
    const GeneratedMobEffect* def = instance.def();
    if (!def || !def->modifiers)
      continue;
    for (int i = 0; i < def->modifier_count; ++i) {
      const GeneratedEffectModifier& modifier = def->modifiers[i];
      if (attribute != modifier.attribute)
        continue;
      EffectAttributeModifier entry;
      // MobEffect.AttributeTemplate.create's id is
      // `minecraft:effect.<effect name>`, which is what lets the client replace
      // the modifier instead of stacking a second one.
      const std::string_view effect_name = def->name;
      entry.id = "minecraft:effect." + std::string(effect_name.substr(effect_name.find(':') + 1));
      entry.attribute = modifier.attribute;
      entry.amount =
          static_cast<double>(modifier.amount) * static_cast<double>(instance.amplifier + 1);
      entry.operation = modifier.operation;
      out->push_back(std::move(entry));
    }
  }
}

double effect_operation_total(const ActiveEffects& effects, std::string_view attribute,
                              uint8_t operation) {
  std::vector<EffectAttributeModifier> modifiers;
  effect_attribute_modifiers(effects, attribute, &modifiers);
  double total = 0.0;
  for (const EffectAttributeModifier& modifier : modifiers)
    if (modifier.operation == operation)
      total += modifier.amount;
  return total;
}

double apply_attribute_modifiers(double base,
                                 const std::vector<EffectAttributeModifier>& modifiers) {
  double value = base;
  for (const EffectAttributeModifier& modifier : modifiers)
    if (modifier.operation == kModifierAddValue)
      value += modifier.amount;
  const double after_add = value;
  for (const EffectAttributeModifier& modifier : modifiers)
    if (modifier.operation == kModifierAddMultipliedBase)
      value += after_add * modifier.amount;
  for (const EffectAttributeModifier& modifier : modifiers)
    if (modifier.operation == kModifierAddMultipliedTotal)
      value *= 1.0 + modifier.amount;
  return value;
}

bool effects_grant_water_breathing(const ActiveEffects& effects) {
  // MobEffectUtil.hasWaterBreathing: Water Breathing *or* Conduit Power.
  for (const MobEffectInstance& instance : effects.all()) {
    const GeneratedMobEffect* def = instance.def();
    if (!def)
      continue;
    const std::string_view name = def->name;
    if (name == "minecraft:water_breathing" || name == "minecraft:conduit_power")
      return true;
  }
  return false;
}

float resistance_damage_multiplier(const ActiveEffects& effects) {
  const GeneratedMobEffect* resistance = mob_effect_by_name("resistance");
  if (!resistance)
    return 1.0f;
  const MobEffectInstance* instance = effects.get(mob_effect_id_of(*resistance));
  if (!instance)
    return 1.0f;
  // LivingEntity.getDamageAfterMagicAbsorb: `(25 - (amplifier + 1) * 5) / 25`,
  // floored at zero so Resistance V (amp 4 -> 5) can null a hit outright.
  const int absorbed = 25 - (instance->amplifier + 1) * 5;
  const float factor = static_cast<float>(absorbed) / 25.0f;
  return factor > 0.0f ? factor : 0.0f;
}

} // namespace mc::world
