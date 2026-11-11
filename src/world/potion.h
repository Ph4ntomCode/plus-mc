// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drinking a potion: the minecraft:potion registry as the engine reads it, and
// what `minecraft:potion_contents` does when its stack is consumed (Phase 3
// milestone G follow-up).
//
// A potion is *not* a consumable whose effects live in `on_consume_effects`.
// They live on the stack, in the potion_contents component, and the component
// applies them itself, because `PotionContents implements ConsumableListener`
// and `Consumable.onConsume` runs the stack's listeners before its own list:
//
//     stack.getAllOfType(ConsumableListener.class).forEach(c -> c.onConsume(...))
//     this.onConsumeEffects.forEach(action -> action.apply(...))
//     stack.consume(1, user)
//
// FoodProperties is the other ConsumableListener, which is why the food and a
// potion's effects are the same step — and why a stack has at most one of the
// two.
//
// `PotionContents.onConsume` is `applyToLivingEntity(user, durationScale)`: for
// each instance of `getAllEffects()` — the named potion's own effects, then the
// stack's custom ones — an instantaneous effect is *applied*
// (`MobEffect.applyInstantaneousEffect`, world/effect.h) and everything else is
// added to the entity's effect set (`LivingEntity.addEffect`). `durationScale`
// is the stack's minecraft:potion_duration_scale component, which this server
// does not model, so it is always 1.0 — and at 1.0 `withScaledDuration` is the
// identity for a finite duration, so the declared duration is the applied one.
//
// The registry table is generated (world/potion_table.h) from the data
// generator's registries.json plus the decompiled Potions.java: `potion` is an
// id the client is sent, and a potion's effects are constructor arguments rather
// than data.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world/item.h"
#include "world/potion_table.h"

namespace mc::world {

// PotionContents.BASE_POTION_COLOR: what a potion with no visible effect of its
// own is drawn with (0xFF385DC6), which is the colour a water bottle's splash
// and cloud carry.
constexpr int32_t kBasePotionColor = -13083194;

// The damage type a potion's instantaneous harm carries, whoever applies it.
// Vanilla's source is `damageSources().indirectMagic(source, owner)` — with the
// drinker as both for a drunk potion, and with the *thrower* as the source for a
// thrown one — and that is what separates it from an `/effect give` of
// instant_damage, which ticks through `damageSources().magic()` (world/effect.h).
constexpr const char* kPotionHarmDamageType = "indirect_magic";

// --- the registry -----------------------------------------------------------

// nullptr for an unknown or malformed name ("healing" is normalized first).
const GeneratedPotion* potion_by_name(std::string_view name);
// nullptr when the id is out of range.
const GeneratedPotion* potion_by_id(int32_t id);
// Registry index (== the wire id), or -1.
int32_t potion_id_of(const GeneratedPotion& potion);

// --- applying the contents --------------------------------------------------

// `PotionContents.hasEffects`: whether anything would be applied at all — the
// named potion's list is non-empty *or* the stack adds effects of its own. Note
// this is not "has an effect this server knows": vanilla branches on the data,
// so an unknown effect still makes a potion a potion.
bool potion_has_effects(const StackPotionContents& contents);

// `PotionContents.getAllEffects`: the named potion's own effects in the order
// `Potion.getEffects()` returns them, then the stack's custom effects. Every
// instance is built the way vanilla's constructor is — ambient false, visible
// true, show_icon true, which is what all 46 potions in the registry are
// registered with and what a data-provided custom effect means when it omits
// them.
std::vector<MobEffectInstance> potion_all_effects(const StackPotionContents& contents);

// `PotionContents.applyToLivingEntity`'s effect half: the instantaneous entries
// become events (the heal, or the harm) and the rest land in `effects`. `out`
// carries both — apply_effect_outcome in the session is the one sink for each,
// so a potion's effects take exactly the path /effect's do (armour, absorption
// and the death path included).
//
// The harm is `indirect_magic`, because vanilla's source is
// `damageSources().indirectMagic(source, owner)` with the drinker as both — not
// the `magic` a ticked instant effect hurts with. Returns whether anything was
// applied at all, which is false for the four potions with no effects.
bool apply_potion_contents(const StackPotionContents& contents, ActiveEffects& effects,
                           EffectOutcome& out);

// --- the /give argument form ------------------------------------------------

// `item[potion_contents={potion:"<name>"}]` — the item argument as vanilla
// spells it, for the one component this server can build. The component list is
// deliberately not a general grammar: `potion_contents` with a `potion` string
// is what a command needs to hand out a potion, and anything else (another
// component, a custom_effects list, a malformed value) is *refused* with an
// error rather than quietly ignored, because a command that silently drops half
// of what it was asked for is worse than one that says no.
//
// `item_id` receives the base item's registry id and `contents` the component;
// `contents` is left empty for a plain item token. Returns false with `*error`
// set to the message to show the player.
bool parse_item_with_potion(std::string_view token, int32_t* item_id,
                            std::optional<StackPotionContents>* contents, std::string* error);

} // namespace mc::world
