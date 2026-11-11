// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drinking a potion: the minecraft:potion registry table's lookups and
// `PotionContents.applyToLivingEntity`. See world/potion.h for what is modelled
// and where the boundary sits.
#include "world/potion.h"

#include <algorithm>
#include <cctype>

namespace mc::world {
namespace {

// The registry key a potion argument may be spelled with: "healing",
// "minecraft:healing", "HEALING". Rejects anything that is not a plain
// identifier, like normalize_item_name.
std::string normalize_potion_name(std::string_view raw) {
  std::string s;
  s.reserve(raw.size() + 10);
  for (char c : raw) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u == ' ' || u == '"')
      continue; // players (and the argument's own quotes) add noise
    s.push_back(static_cast<char>(std::tolower(u)));
  }
  if (s.empty())
    return s;
  if (s.find(':') == std::string::npos)
    s.insert(0, "minecraft:");
  bool colon = false;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == ':') {
      if (colon || i == 0 || i + 1 == s.size())
        return {};
      colon = true;
      continue;
    }
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    if (!ok)
      return {};
  }
  return s;
}

// Strips a pair of double quotes, which is how a value carrying a colon has to
// be spelled in a command argument (`potion:"minecraft:healing"`). Unquoted is
// accepted too: Brigadier's string reader stops at the field's own terminator,
// so both forms are the same string.
std::string unquote(std::string text) {
  if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
    return text.substr(1, text.size() - 2);
  return text;
}

// The single component in `[...]`: `potion_contents={potion:"..."}`, whose
// braced field list is written into `fields`. Any other component — including a
// second one after a comma — is refused, because this command form does not
// implement it and dropping it silently is worse than saying no.
bool parse_potion_component(std::string_view inside, std::string* fields) {
  const std::string text(inside);
  const std::string prefix = "potion_contents=";
  if (text.compare(0, prefix.size(), prefix) != 0)
    return false;
  const std::string body = text.substr(prefix.size());
  if (body.size() < 2 || body.front() != '{' || body.back() != '}')
    return false;
  *fields = body.substr(1, body.size() - 2);
  return true;
}

// The braced field list's own single field: `potion:"minecraft:healing"`.
// `StackPotionContents` keeps the potion and the custom effects and nothing
// else, so a list that names any other field is refused here rather than
// half-applied.
bool parse_potion_field(std::string_view fields, std::string* name) {
  const std::string text(fields);
  bool quoted = false;
  for (char c : text) {
    if (c == '"')
      quoted = !quoted;
    else if (c == ',' && !quoted)
      return false; // a second field, which this command would have to drop
  }
  const std::string prefix = "potion";
  if (text.size() < prefix.size() + 2 || text.compare(0, prefix.size(), prefix) != 0 ||
      text[prefix.size()] != ':')
    return false;
  *name = unquote(text.substr(prefix.size() + 1));
  return true;
}

} // namespace

const GeneratedPotion* potion_by_name(std::string_view name) {
  const std::string normalized = normalize_potion_name(name);
  if (normalized.empty())
    return nullptr;
  for (int i = 0; i < kPotionCount; ++i)
    if (normalized == kPotions[i].name)
      return &kPotions[i];
  return nullptr;
}

const GeneratedPotion* potion_by_id(int32_t id) {
  if (id < 0 || id >= kPotionCount)
    return nullptr;
  return &kPotions[id];
}

int32_t potion_id_of(const GeneratedPotion& potion) {
  return potion.id;
}

bool potion_has_effects(const StackPotionContents& contents) {
  if (!contents.custom_effects.empty())
    return true;
  const GeneratedPotion* potion = potion_by_id(contents.potion);
  return potion != nullptr && potion->count > 0;
}

std::vector<MobEffectInstance> potion_all_effects(const StackPotionContents& contents) {
  std::vector<MobEffectInstance> effects;
  if (const GeneratedPotion* potion = potion_by_id(contents.potion)) {
    effects.reserve(static_cast<size_t>(potion->count) + contents.custom_effects.size());
    for (int i = 0; i < potion->count; ++i) {
      const GeneratedPotionEffect& row = kPotionEffects[potion->first + i];
      MobEffectInstance instance;
      instance.effect = row.effect;
      instance.duration = row.duration;
      instance.amplifier = row.amplifier;
      // `new MobEffectInstance(effect, duration[, amplifier])`'s own defaults:
      // ambient false, visible true, show_icon true.
      instance.ambient = false;
      instance.visible = true;
      instance.show_icon = true;
      effects.push_back(std::move(instance));
    }
  }
  // `Iterables.concat(potion.getEffects(), customEffects)`: the custom effects
  // come *after* the potion's own, which is the order they land in.
  for (const MobEffectInstance& instance : contents.custom_effects)
    effects.push_back(instance);
  return effects;
}

bool apply_potion_contents(const StackPotionContents& contents, ActiveEffects& effects,
                           EffectOutcome& out) {
  bool applied = false;
  for (const MobEffectInstance& instance : potion_all_effects(contents)) {
    const GeneratedMobEffect* def = mob_effect_by_id(instance.effect);
    if (!def)
      continue; // an id this server does not know: nothing to apply
    if (def->instantaneous) {
      // `effect.getEffect().value().applyInstantaneousEffect(serverLevel,
      // player, player, entity, effect.getAmplifier(), 1.0)` — the drinker is
      // both the source and the owner of the damage.
      apply_instant_effect(*def, instance.amplifier, kPotionHarmDamageType, out);
      applied = true;
      continue;
    }
    // `entity.addEffect(instance)`: an effect already running is merged, and the
    // outcome carries whichever packet that needs.
    if (effects.add(instance, out))
      applied = true;
  }
  return applied;
}

bool parse_item_with_potion(std::string_view token, int32_t* item_id,
                            std::optional<StackPotionContents>* contents, std::string* error) {
  if (contents)
    contents->reset();
  const size_t open = token.find('[');
  const std::string base(token.substr(0, open == std::string_view::npos ? token.size() : open));
  const std::optional<int32_t> id = ItemRegistry::instance().id_of(base);
  if (!id) {
    if (error)
      *error = "Unknown item '" + std::string(token) + "'";
    return false;
  }
  if (open == std::string_view::npos) {
    if (item_id)
      *item_id = *id;
    return true;
  }
  if (token.back() != ']' || open + 1 >= token.size()) {
    if (error)
      *error = "Malformed item argument '" + std::string(token) + "'";
    return false;
  }
  std::string fields;
  if (!parse_potion_component(token.substr(open + 1, token.size() - open - 2), &fields)) {
    if (error)
      *error = "Unsupported component in '" + std::string(token) +
               "' (only potion_contents={...} is understood)";
    return false;
  }
  // An empty field list names no potion, which is exactly what a water bottle's
  // contents are — a component that is present and points at nothing.
  StackPotionContents built;
  if (!fields.empty()) {
    std::string potion_name;
    if (!parse_potion_field(fields, &potion_name)) {
      if (error)
        *error = "Unsupported potion_contents field in '" + std::string(token) +
                 "' (only potion:\"<name>\" is understood)";
      return false;
    }
    const GeneratedPotion* potion = potion_by_name(potion_name);
    if (!potion) {
      if (error)
        *error = "Unknown potion '" + potion_name + "'";
      return false;
    }
    built.potion = potion_id_of(*potion);
  }
  if (item_id)
    *item_id = *id;
  if (contents)
    *contents = std::move(built);
  return true;
}

} // namespace mc::world
