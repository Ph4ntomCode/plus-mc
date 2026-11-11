// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for drinking a potion: the generated minecraft:potion table,
// `minecraft:potion_contents` as a stack component, and what
// `PotionContents.applyToLivingEntity` does with it.
//
//   * the table: 46 potions with dense ids, the four with no effects, the
//     instant ones (`healing`, `harming` and their strong forms), the long/strong
//     durations, and `turtle_master`'s two effects in the order the potion
//     declares them;
//   * `getAllEffects`: the potion's own effects first, then the stack's custom
//     ones, each built with `MobEffectInstance`'s constructor defaults;
//   * `apply_potion_contents`: an instantaneous effect becomes an event and never
//     lands in the effect set (the heal is `4 << amplifier` and the harm
//     `6 << amplifier`, and the harm is *indirect_magic* because a drunk potion's
//     source is the drinker), a lasting one is added (and merged when one is
//     already running), and a water bottle applies nothing at all;
//   * the component's wire codec, both directions: the trusted patch
//     write_item_stack emits, and the delimited value a client sends;
//   * `/give`'s `item[potion_contents={potion:"..."}]` argument form, including
//     everything it refuses.
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "world/damage.h"
#include "world/effect.h"
#include "world/item.h"
#include "world/item_registry.h"
#include "world/potion.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

#define CHECK_EQ(a, b)                                                                             \
  do {                                                                                             \
    const long long va = static_cast<long long>(a), vb = static_cast<long long>(b);                \
    if (va != vb) {                                                                                \
      std::fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, va, vb); \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

int32_t effect_id(const char* name) {
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  return def ? mob_effect_id_of(*def) : -1;
}

const GeneratedPotion* potion(const char* name) {
  return potion_by_name(name);
}

// One potion's effects as (effect, duration, amplifier) triples.
std::vector<std::vector<int32_t>> effects_of(const char* name) {
  std::vector<std::vector<int32_t>> out;
  const GeneratedPotion* p = potion(name);
  if (!p)
    return out;
  for (int i = 0; i < p->count; ++i)
    out.push_back({kPotionEffects[p->first + i].effect, kPotionEffects[p->first + i].duration,
                   kPotionEffects[p->first + i].amplifier});
  return out;
}

StackPotionContents contents_of(const char* name) {
  StackPotionContents contents;
  const GeneratedPotion* p = potion(name);
  contents.potion = p ? potion_id_of(*p) : -1;
  return contents;
}

MobEffectInstance custom(const char* effect_name, int duration, int amplifier) {
  MobEffectInstance instance;
  instance.effect = effect_id(effect_name);
  instance.duration = duration;
  instance.amplifier = amplifier;
  instance.ambient = false;
  instance.visible = true;
  instance.show_icon = true;
  return instance;
}

// The delimited (client-sent) form of one component value: `[varint id][varint
// length][value]`, which is what ItemStack.DELIMITED_STREAM_CODEC wraps every
// patch entry in so a server can skip what it does not model.
std::vector<uint8_t> delimited_component(int32_t id, const std::vector<uint8_t>& value) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(id));
  w.varint(static_cast<uint32_t>(value.size()));
  w.bytes(value);
  return std::move(w).take();
}

void append_varint(std::vector<uint8_t>* out, uint32_t value) {
  do {
    uint8_t byte = static_cast<uint8_t>(value & 0x7f);
    value >>= 7;
    if (value)
      byte |= 0x80;
    out->push_back(byte);
  } while (value);
}

void append_u32(std::vector<uint8_t>* out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out->push_back(static_cast<uint8_t>(value >> shift));
}

std::vector<uint8_t> read_stack(int32_t item, int32_t count,
                                const std::vector<std::vector<uint8_t>>& components) {
  mc::net::Writer w;
  w.varint(static_cast<uint32_t>(count));
  w.varint(static_cast<uint32_t>(item));
  w.varint(static_cast<uint32_t>(components.size()));
  w.varint(0); // removed
  for (const std::vector<uint8_t>& component : components)
    w.bytes(component);
  return std::move(w).take();
}

// --- the table ---------------------------------------------------------------

void test_table() {
  CHECK_EQ(kPotionCount, 46);
  CHECK_EQ(kPotionEffectCount, 45);
  // Dense ids in registration order, which is what makes the table index the
  // wire id.
  for (int i = 0; i < kPotionCount; ++i) {
    CHECK_EQ(kPotions[i].id, i);
    CHECK(potion_by_id(i) == &kPotions[i]);
    CHECK(std::string(potion_by_id(i)->name).rfind("minecraft:", 0) == 0);
  }
  CHECK(potion_by_id(-1) == nullptr);
  CHECK(potion_by_id(kPotionCount) == nullptr);

  // The four potions with nothing in them, and the one shape that makes the
  // count column matter.
  for (const char* name :
       {"minecraft:water", "minecraft:mundane", "minecraft:thick", "minecraft:awkward"}) {
    const GeneratedPotion* p = potion(name);
    CHECK(p != nullptr);
    if (p)
      CHECK_EQ(p->count, 0);
  }
  // water is id 0, so a component with `potion: 0` names it rather than nothing.
  CHECK(potion("minecraft:water") == potion_by_id(0));

  // The registry's own order, as Potions.java declares it.
  CHECK_EQ(kPotions[0].id, 0);
  CHECK(std::string(potion("minecraft:night_vision")->name) ==
        std::string("minecraft:night_vision"));
  CHECK(std::string(potion("minecraft:long_night_vision")->name) ==
        std::string("minecraft:long_night_vision"));

  // Durations and amplifiers, including the two effects turtle_master applies
  // (Slowness III then Resistance II) and the strong form's bigger numbers.
  const std::vector<std::vector<int32_t>> swiftness = effects_of("minecraft:swiftness");
  CHECK_EQ(swiftness.size(), 1);
  if (swiftness.size() == 1) {
    CHECK_EQ(swiftness[0][0], effect_id("minecraft:speed"));
    CHECK_EQ(swiftness[0][1], 3600);
    CHECK_EQ(swiftness[0][2], 0);
  }
  const std::vector<std::vector<int32_t>> long_swiftness = effects_of("minecraft:long_swiftness");
  CHECK_EQ(long_swiftness.size(), 1);
  if (long_swiftness.size() == 1)
    CHECK_EQ(long_swiftness[0][1], 9600);
  const std::vector<std::vector<int32_t>> strong_swiftness =
      effects_of("minecraft:strong_swiftness");
  CHECK_EQ(strong_swiftness.size(), 1);
  if (strong_swiftness.size() == 1) {
    CHECK_EQ(strong_swiftness[0][1], 1800);
    CHECK_EQ(strong_swiftness[0][2], 1);
  }
  const std::vector<std::vector<int32_t>> turtle = effects_of("minecraft:turtle_master");
  CHECK_EQ(turtle.size(), 2);
  if (turtle.size() == 2) {
    CHECK_EQ(turtle[0][0], effect_id("minecraft:slowness"));
    CHECK_EQ(turtle[0][1], 400);
    CHECK_EQ(turtle[0][2], 3);
    CHECK_EQ(turtle[1][0], effect_id("minecraft:resistance"));
    CHECK_EQ(turtle[1][1], 400);
    CHECK_EQ(turtle[1][2], 2);
  }
  const std::vector<std::vector<int32_t>> strong_turtle =
      effects_of("minecraft:strong_turtle_master");
  CHECK_EQ(strong_turtle.size(), 2);
  if (strong_turtle.size() == 2) {
    CHECK_EQ(strong_turtle[0][2], 5);
    CHECK_EQ(strong_turtle[1][2], 3);
  }

  // The instantaneous ones: duration 1 and the effect the effect table knows.
  const std::vector<std::vector<int32_t>> healing = effects_of("minecraft:healing");
  CHECK_EQ(healing.size(), 1);
  if (healing.size() == 1) {
    CHECK_EQ(healing[0][0], effect_id("minecraft:instant_health"));
    CHECK_EQ(healing[0][1], 1);
    CHECK_EQ(healing[0][2], 0);
  }
  const std::vector<std::vector<int32_t>> strong_healing = effects_of("minecraft:strong_healing");
  CHECK_EQ(strong_healing.size(), 1);
  if (strong_healing.size() == 1)
    CHECK_EQ(strong_healing[0][2], 1);
  const std::vector<std::vector<int32_t>> harming = effects_of("minecraft:harming");
  CHECK_EQ(harming.size(), 1);
  if (harming.size() == 1)
    CHECK_EQ(harming[0][0], effect_id("minecraft:instant_damage"));
  const std::vector<std::vector<int32_t>> strong_harming = effects_of("minecraft:strong_harming");
  CHECK_EQ(strong_harming.size(), 1);
  if (strong_harming.size() == 1)
    CHECK_EQ(strong_harming[0][2], 1);

  // Every effect id in the table is an effect this server knows, which is the
  // property the two tables being generated from the same registry earns.
  for (int i = 0; i < kPotionEffectCount; ++i)
    CHECK(mob_effect_by_id(kPotionEffects[i].effect) != nullptr);

  // Name lookup normalizes the way item names do.
  CHECK(potion("healing") == potion("minecraft:healing"));
  CHECK(potion("MINECRAFT:HEALING") == potion("minecraft:healing"));
  CHECK(potion("minecraft:not_a_potion") == nullptr);
  CHECK(potion("not a potion!") == nullptr);
  CHECK(potion("") == nullptr);
}

// --- getAllEffects -----------------------------------------------------------

void test_all_effects() {
  const StackPotionContents healing = contents_of("minecraft:healing");
  const std::vector<MobEffectInstance> effects = potion_all_effects(healing);
  CHECK_EQ(effects.size(), 1);
  if (effects.size() == 1) {
    CHECK_EQ(effects[0].effect, effect_id("minecraft:instant_health"));
    CHECK_EQ(effects[0].duration, 1);
    CHECK_EQ(effects[0].amplifier, 0);
    // `new MobEffectInstance(effect, duration)`'s own defaults.
    CHECK(!effects[0].ambient);
    CHECK(effects[0].visible);
    CHECK(effects[0].show_icon);
    CHECK(effects[0].hidden.empty());
  }

  // The potion's own effects, then the stack's: vanilla concatenates them in
  // that order (`Iterables.concat(potion.getEffects(), customEffects)`).
  StackPotionContents mixed = contents_of("minecraft:swiftness");
  mixed.custom_effects.push_back(custom("minecraft:poison", 40, 1));
  const std::vector<MobEffectInstance> both = potion_all_effects(mixed);
  CHECK_EQ(both.size(), 2);
  if (both.size() == 2) {
    CHECK_EQ(both[0].effect, effect_id("minecraft:speed"));
    CHECK_EQ(both[0].duration, 3600);
    CHECK_EQ(both[1].effect, effect_id("minecraft:poison"));
    CHECK_EQ(both[1].duration, 40);
    CHECK_EQ(both[1].amplifier, 1);
  }

  // A component with no potion at all still has its custom effects — which is
  // what a bottle filled with a custom effect (and no named potion) is.
  StackPotionContents custom_only;
  custom_only.custom_effects.push_back(custom("minecraft:strength", 200, 2));
  const std::vector<MobEffectInstance> only = potion_all_effects(custom_only);
  CHECK_EQ(only.size(), 1);
  if (only.size() == 1)
    CHECK_EQ(only[0].effect, effect_id("minecraft:strength"));

  // A water bottle has nothing: no potion effects, no custom ones.
  CHECK(potion_all_effects(contents_of("minecraft:water")).empty());
  CHECK(potion_all_effects(StackPotionContents{}).empty());
}

// --- applyToLivingEntity -----------------------------------------------------

// The one event a treatment produced, or a zeroed one.
EffectEvent only_event(const EffectOutcome& out) {
  if (out.events.size() == 1)
    return out.events[0];
  std::fprintf(stderr, "  wanted one event, got %zu\n", out.events.size());
  return EffectEvent{};
}

void test_instant_effects() {
  // Healing I: `4 << 0` health, and *nothing* added to the effect set — an
  // instant effect is applied, not installed.
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:healing"), effects, out));
    CHECK_EQ(out.events.size(), 1);
    CHECK_EQ(out.added.size(), 0);
    CHECK(effects.empty());
    const EffectEvent event = only_event(out);
    CHECK(event.kind == EffectEventKind::kHeal);
    CHECK_EQ(event.amount, 4);
    CHECK(event.damage_type == nullptr);
  }
  // Strong Healing: `4 << 1` = 8.
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:strong_healing"), effects, out));
    CHECK_EQ(only_event(out).amount, 8);
  }
  // Harming: `6 << 0` damage, and the type is indirect_magic rather than the
  // magic a ticked instant effect hurts with.
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:harming"), effects, out));
    const EffectEvent event = only_event(out);
    CHECK(event.kind == EffectEventKind::kDamage);
    CHECK_EQ(event.amount, 6);
    CHECK(event.damage_type != nullptr);
    if (event.damage_type)
      CHECK(std::string(event.damage_type) == std::string("indirect_magic"));
    CHECK(effects.empty());
    // ...and the damage type really is in the damage table, so the session's
    // damage_player will accept it.
    CHECK(damage_type_by_name("indirect_magic") != nullptr);
  }
  // Strong Harming: `6 << 1` = 12.
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:strong_harming"), effects, out));
    CHECK_EQ(only_event(out).amount, 12);
  }
  // A custom instantaneous effect takes the same path (the component's
  // custom_effects are applied by the same isInstantaneous test).
  {
    ActiveEffects effects;
    EffectOutcome out;
    StackPotionContents contents;
    contents.custom_effects.push_back(custom("minecraft:instant_health", 1, 1));
    CHECK(apply_potion_contents(contents, effects, out));
    CHECK_EQ(only_event(out).amount, 8);
    CHECK(effects.empty());
  }
}

void test_lasting_effects() {
  // Swiftness: one instance lands, with the table's duration and amplifier, and
  // the outcome says it was *added* (onEffectAdded's blend packet).
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:swiftness"), effects, out));
    CHECK(out.events.empty());
    CHECK_EQ(out.added.size(), 1);
    if (out.added.size() == 1)
      CHECK_EQ(out.added[0], effect_id("minecraft:speed"));
    const MobEffectInstance* landed = effects.get(effect_id("minecraft:speed"));
    CHECK(landed != nullptr);
    if (landed) {
      CHECK_EQ(landed->duration, 3600);
      CHECK_EQ(landed->amplifier, 0);
      CHECK(landed->visible);
      CHECK(landed->show_icon);
      CHECK(!landed->ambient);
    }
  }
  // Turtle Master: both effects, and the set holds each once.
  {
    ActiveEffects effects;
    EffectOutcome out;
    CHECK(apply_potion_contents(contents_of("minecraft:turtle_master"), effects, out));
    CHECK_EQ(effects.size(), 2);
    CHECK(effects.has(effect_id("minecraft:slowness")));
    CHECK(effects.has(effect_id("minecraft:resistance")));
    const MobEffectInstance* slowness = effects.get(effect_id("minecraft:slowness"));
    if (slowness)
      CHECK_EQ(slowness->amplifier, 3);
  }
  // A second drink *merges* rather than stacking: a running Swiftness is
  // extended to Long Swiftness' 9600 ticks, one instance, and the outcome
  // reports an update rather than a second add.
  {
    ActiveEffects effects;
    EffectOutcome first;
    apply_potion_contents(contents_of("minecraft:swiftness"), effects, first);
    EffectOutcome ticked;
    effects.tick(0, EffectTickContext{}, ticked); // one tick of the first drink
    EffectOutcome second;
    apply_potion_contents(contents_of("minecraft:long_swiftness"), effects, second);
    CHECK_EQ(effects.size(), 1);
    CHECK_EQ(second.added.size(), 0);
    CHECK_EQ(second.updated.size(), 1);
    if (second.updated.size() == 1)
      CHECK_EQ(second.updated[0], effect_id("minecraft:speed"));
    const MobEffectInstance* landed = effects.get(effect_id("minecraft:speed"));
    CHECK(landed != nullptr);
    if (landed) {
      CHECK_EQ(landed->duration, 9600);
      CHECK_EQ(landed->amplifier, 0);
    }
  }
  // A *stronger* drink takes over from a running weaker one and keeps the weaker
  // instance underneath it (`MobEffectInstance.update`'s hidden chain), which is
  // an update the client is told about.
  {
    ActiveEffects effects;
    EffectOutcome first;
    apply_potion_contents(contents_of("minecraft:swiftness"), effects, first);
    EffectOutcome second;
    apply_potion_contents(contents_of("minecraft:strong_swiftness"), effects, second);
    CHECK_EQ(second.updated.size(), 1);
    const MobEffectInstance* landed = effects.get(effect_id("minecraft:speed"));
    CHECK(landed != nullptr);
    if (landed) {
      CHECK_EQ(landed->amplifier, 1);
      CHECK_EQ(landed->duration, 1800);
      CHECK_EQ(landed->hidden.size(), 1);
      if (!landed->hidden.empty()) {
        CHECK_EQ(landed->hidden[0].amplifier, 0);
        CHECK_EQ(landed->hidden[0].duration, 3600);
      }
    }
  }
  // ...and the reverse changes only the hidden chain, which no packet carries:
  // `update` returns false without touching the visible instance, so a weaker
  // drink under a stronger one sends nothing at all.
  {
    ActiveEffects effects;
    EffectOutcome first;
    apply_potion_contents(contents_of("minecraft:strong_swiftness"), effects, first);
    EffectOutcome second;
    CHECK(!apply_potion_contents(contents_of("minecraft:swiftness"), effects, second));
    CHECK_EQ(second.added.size(), 0);
    CHECK_EQ(second.updated.size(), 0);
    const MobEffectInstance* landed = effects.get(effect_id("minecraft:speed"));
    CHECK(landed != nullptr);
    if (landed) {
      CHECK_EQ(landed->amplifier, 1);
      CHECK_EQ(landed->duration, 1800);
      CHECK_EQ(landed->hidden.size(), 1);
      if (!landed->hidden.empty())
        CHECK_EQ(landed->hidden[0].duration, 3600);
    }
    // An *identical* drink is the same case: nothing changes, nothing is sent.
    EffectOutcome third;
    CHECK(!apply_potion_contents(contents_of("minecraft:strong_swiftness"), effects, third));
    CHECK_EQ(third.updated.size(), 0);
  }
  // The custom effects are applied *after* the potion's own, so an instant one
  // from the potion fires before a lasting custom one lands.
  {
    ActiveEffects effects;
    EffectOutcome out;
    StackPotionContents contents = contents_of("minecraft:healing");
    contents.custom_effects.push_back(custom("minecraft:speed", 100, 0));
    CHECK(apply_potion_contents(contents, effects, out));
    CHECK_EQ(out.events.size(), 1);
    CHECK_EQ(out.added.size(), 1);
    CHECK(effects.has(effect_id("minecraft:speed")));
    CHECK_EQ(effects.size(), 1);
  }
}

void test_nothing_to_apply() {
  ActiveEffects effects;
  EffectOutcome out;
  // A water bottle's contents are present and point at a potion with no effects.
  CHECK(!apply_potion_contents(contents_of("minecraft:water"), effects, out));
  CHECK(out.events.empty());
  CHECK(out.added.empty());
  CHECK(out.updated.empty());
  CHECK(effects.empty());
  // The same for a component that names no potion at all, and for one that
  // names a potion this table does not have.
  StackPotionContents none;
  CHECK(!apply_potion_contents(none, effects, out));
  none.potion = kPotionCount + 5;
  CHECK(!apply_potion_contents(none, effects, out));
  // ...but a custom effect on top of a water bottle still applies (the two
  // lists are concatenated, not alternatives).
  StackPotionContents custom_only;
  custom_only.custom_effects.push_back(custom("minecraft:regeneration", 50, 0));
  CHECK(apply_potion_contents(custom_only, effects, out));
  CHECK(effects.has(effect_id("minecraft:regeneration")));
}

// --- the component on the wire -----------------------------------------------

void test_write_component() {
  const int32_t potion_item = ItemRegistry::instance().id_of("minecraft:potion").value_or(-1);
  ItemStack stack = ItemStack::of_id(potion_item, 1);
  StackPotionContents drink = contents_of("minecraft:healing");
  drink.custom_color = 0x7f;
  drink.custom_name = "test";
  stack.potion_contents = std::move(drink);
  mc::net::Writer w;
  write_item_stack(w, stack);
  const std::vector<uint8_t> bytes = std::move(w).take();

  std::vector<uint8_t> expected;
  append_varint(&expected, 1); // count
  append_varint(&expected, static_cast<uint32_t>(potion_item));
  append_varint(&expected, 1);  // one added component
  append_varint(&expected, 0);  // none removed
  append_varint(&expected, 53); // minecraft:potion_contents
  expected.push_back(1);        // the potion holder is present
  append_varint(&expected, 24); // minecraft:healing
  expected.push_back(1);        // custom_color present
  append_u32(&expected, 0x7f);  // ...the colour is a fixed-width INT
  append_varint(&expected, 0);  // custom_effects: none
  expected.push_back(1);        // custom_name present
  expected.push_back(4);        // ...four bytes of UTF-8
  expected.insert(expected.end(), {'t', 'e', 's', 't'});
  CHECK(bytes == expected);
  if (bytes != expected) {
    std::fprintf(stderr, "  got  ");
    for (uint8_t b : bytes)
      std::fprintf(stderr, "%02x ", b);
    std::fprintf(stderr, "\n  want ");
    for (uint8_t b : expected)
      std::fprintf(stderr, "%02x ", b);
    std::fprintf(stderr, "\n");
  }
  CHECK_EQ(kComponentPotionContentsId, 53);

  // A stack with no contents writes no component at all: a potion is a plain
  // item until something fills it.
  const ItemStack plain = ItemStack::of_id(potion_item, 1);
  mc::net::Writer plain_w;
  write_item_stack(plain_w, plain);
  const std::vector<uint8_t> plain_bytes = std::move(plain_w).take();
  std::vector<uint8_t> plain_expected;
  append_varint(&plain_expected, 1);
  append_varint(&plain_expected, static_cast<uint32_t>(potion_item));
  append_varint(&plain_expected, 0);
  append_varint(&plain_expected, 0);
  CHECK(plain_bytes == plain_expected);

  // The custom-effect form, with the hidden chain the merge produces: the list
  // is `[varint effect][varint amplifier][varint duration][ambient][visible]
  // [show_icon][hidden present]`, the hidden layer carrying no effect id.
  MobEffectInstance instance = custom("minecraft:poison", 40, 1);
  MobEffectInstance hidden = custom("minecraft:poison", 10, 0);
  instance.hidden.push_back(hidden);
  ItemStack custom_stack = ItemStack::of_id(potion_item, 1);
  StackPotionContents contents;
  contents.custom_effects.push_back(instance);
  custom_stack.potion_contents = contents;
  mc::net::Writer custom_w;
  write_item_stack(custom_w, custom_stack);
  const std::vector<uint8_t> custom_bytes = std::move(custom_w).take();
  std::vector<uint8_t> custom_expected;
  append_varint(&custom_expected, 1);
  append_varint(&custom_expected, static_cast<uint32_t>(potion_item));
  append_varint(&custom_expected, 1);
  append_varint(&custom_expected, 0);
  append_varint(&custom_expected, 53);
  custom_expected.push_back(0);        // no potion holder
  custom_expected.push_back(0);        // custom_color absent
  append_varint(&custom_expected, 1);  // one custom effect
  append_varint(&custom_expected, 18); // minecraft:poison
  append_varint(&custom_expected, 1);  // amplifier
  append_varint(&custom_expected, 40); // duration
  custom_expected.push_back(0);        // ambient
  custom_expected.push_back(1);        // visible
  custom_expected.push_back(1);        // show_icon
  custom_expected.push_back(1);        // a hidden effect follows
  append_varint(&custom_expected, 0);  // the hidden layer's amplifier
  append_varint(&custom_expected, 10); // ...its duration
  custom_expected.push_back(0);        // ...ambient
  custom_expected.push_back(1);        // ...visible
  custom_expected.push_back(1);        // ...show_icon
  custom_expected.push_back(0);        // ...and nothing under it
  custom_expected.push_back(0);        // custom_name absent
  CHECK(custom_bytes == custom_expected);
  if (custom_bytes != custom_expected) {
    std::fprintf(stderr, "  custom got  ");
    for (uint8_t b : custom_bytes)
      std::fprintf(stderr, "%02x ", b);
    std::fprintf(stderr, "\n  custom want ");
    for (uint8_t b : custom_expected)
      std::fprintf(stderr, "%02x ", b);
    std::fprintf(stderr, "\n");
  }
}

void test_read_component() {
  const int32_t potion_item = ItemRegistry::instance().id_of("minecraft:potion").value_or(-1);
  // The delimited value a client sends for `{potion:"minecraft:harming"}`.
  const std::vector<uint8_t> value = {1, 26, 0, 0, 0};
  const std::vector<uint8_t> stack_bytes =
      read_stack(potion_item, 1, {delimited_component(53, value)});
  ItemStack stack;
  mc::net::Reader r(stack_bytes);
  CHECK(read_item_stack(r, &stack));
  CHECK_EQ(stack.item, potion_item);
  CHECK_EQ(stack.count, 1);
  CHECK(stack.potion_contents.has_value());
  if (stack.potion_contents) {
    CHECK_EQ(stack.potion_contents->potion, 26);
    CHECK(stack.potion_contents->custom_effects.empty());
    CHECK(!stack.potion_contents->empty());
  }

  // A custom effect with a hidden layer, the custom colour and the custom name
  // all present: the two display-only fields have to be *parsed* for the effects
  // list that sits between them to stay aligned.
  std::vector<uint8_t> custom_value = {
      0, // no potion
      1, // custom_color present
  };
  append_u32(&custom_value, 0x7f);   // ...the colour's fixed-width INT
  append_varint(&custom_value, 1);   // one custom effect
  append_varint(&custom_value, 0);   // minecraft:speed
  append_varint(&custom_value, 2);   // amplifier
  append_varint(&custom_value, 200); // duration
  custom_value.push_back(0);         // ambient
  custom_value.push_back(1);         // visible
  custom_value.push_back(1);         // show_icon
  custom_value.push_back(1);         // a hidden layer follows
  append_varint(&custom_value, 0);   // ...its amplifier
  append_varint(&custom_value, 50);  // ...its duration
  custom_value.push_back(0);         // ...ambient
  custom_value.push_back(1);         // ...visible
  custom_value.push_back(1);         // ...show_icon
  custom_value.push_back(0);         // ...nothing under it
  custom_value.push_back(1);         // custom_name present
  custom_value.push_back(4);         // ...four bytes of UTF-8
  custom_value.insert(custom_value.end(), {'t', 'e', 's', 't'});
  const std::vector<uint8_t> custom_stack_bytes =
      read_stack(potion_item, 1, {delimited_component(53, custom_value)});
  ItemStack custom_stack;
  mc::net::Reader custom_reader(custom_stack_bytes);
  CHECK(read_item_stack(custom_reader, &custom_stack));
  CHECK(custom_stack.potion_contents.has_value());
  if (custom_stack.potion_contents) {
    CHECK_EQ(custom_stack.potion_contents->potion, -1);
    CHECK(custom_stack.potion_contents->custom_color.has_value());
    if (custom_stack.potion_contents->custom_color)
      CHECK_EQ(*custom_stack.potion_contents->custom_color, 0x7f);
    CHECK(custom_stack.potion_contents->custom_name == std::optional<std::string>("test"));
    CHECK_EQ(custom_stack.potion_contents->custom_effects.size(), 1);
    if (!custom_stack.potion_contents->custom_effects.empty()) {
      const MobEffectInstance& instance = custom_stack.potion_contents->custom_effects[0];
      CHECK_EQ(instance.effect, effect_id("minecraft:speed"));
      CHECK_EQ(instance.amplifier, 2);
      CHECK_EQ(instance.duration, 200);
      CHECK_EQ(instance.hidden.size(), 1);
      if (!instance.hidden.empty()) {
        CHECK_EQ(instance.hidden[0].duration, 50);
        CHECK_EQ(instance.hidden[0].amplifier, 0);
        // The hidden layer's effect is the instance's own: the wire does not
        // repeat it, because it is always the same effect.
        CHECK_EQ(instance.hidden[0].effect, instance.effect);
      }
    }
  }

  // The empty contents a water bottle has: present, no potion, no effects. (The
  // bytes are held in a named vector because Reader takes a *reference*: an
  // inline temporary would dangle.)
  const std::vector<uint8_t> empty_value = {0, 0, 0, 0};
  const std::vector<uint8_t> empty_stack_bytes =
      read_stack(potion_item, 1, {delimited_component(53, empty_value)});
  ItemStack empty_stack;
  mc::net::Reader empty_reader(empty_stack_bytes);
  CHECK(read_item_stack(empty_reader, &empty_stack));
  CHECK(empty_stack.potion_contents.has_value());
  if (empty_stack.potion_contents)
    CHECK(empty_stack.potion_contents->empty());

  // Truncated values fail rather than half-reading: every field of the
  // component is read, so a value that stops early is a malformed packet.
  for (size_t cut = 1; cut < value.size(); ++cut) {
    const std::vector<uint8_t> short_value(value.begin(), value.begin() + static_cast<long>(cut));
    const std::vector<uint8_t> truncated_bytes =
        read_stack(potion_item, 1, {delimited_component(53, short_value)});
    ItemStack truncated;
    mc::net::Reader truncated_reader(truncated_bytes);
    CHECK(!read_item_stack(truncated_reader, &truncated));
  }
  // A hostile custom-effect count is refused before the vector grows, and a
  // nested hidden chain past the cap is refused rather than followed.
  {
    std::vector<uint8_t> huge = {0, 0};
    append_varint(&huge, 0xffffffffu); // 2^32-1 effects, which the cap refuses
    huge.push_back(0);
    const std::vector<uint8_t> huge_bytes =
        read_stack(potion_item, 1, {delimited_component(53, huge)});
    ItemStack out;
    mc::net::Reader reader(huge_bytes);
    CHECK(!read_item_stack(reader, &out));
  }
  {
    // absent potion, absent colour, one effect (speed, amplifier 0, duration 1,
    // ambient 0, visible 1, show_icon 1) whose details say "a layer follows".
    std::vector<uint8_t> deep = {0, 0, 1, 0, 0, 1, 0, 1, 1, 1};
    const uint8_t layer[] = {0, 10, 0, 1, 1, 1}; // amp, dur, ambient, visible, icon, hidden
    for (int i = 0; i < 40; ++i)
      deep.insert(deep.end(), layer, layer + 6);
    deep.push_back(0); // custom_name absent
    const std::vector<uint8_t> deep_bytes =
        read_stack(potion_item, 1, {delimited_component(53, deep)});
    ItemStack out;
    mc::net::Reader reader(deep_bytes);
    CHECK(!read_item_stack(reader, &out));
  }
}

void test_same_item() {
  const int32_t potion_item = ItemRegistry::instance().id_of("minecraft:potion").value_or(-1);
  const ItemStack plain = ItemStack::of_id(potion_item, 1);
  ItemStack healing = plain;
  healing.potion_contents = contents_of("minecraft:healing");
  ItemStack healing_too = plain;
  healing_too.potion_contents = contents_of("minecraft:healing");
  ItemStack harming = plain;
  harming.potion_contents = contents_of("minecraft:harming");

  CHECK(plain.same_item(plain));
  CHECK(healing.same_item(healing_too));
  // A potion of healing is not a water bottle: the component is part of the
  // item's identity (isSameItemSameComponents).
  CHECK(!plain.same_item(healing));
  CHECK(!healing.same_item(harming));

  // Two components with the same custom effect are the same; a different
  // duration is not.
  ItemStack c1 = plain;
  StackPotionContents contents1;
  contents1.custom_effects.push_back(custom("minecraft:speed", 100, 0));
  c1.potion_contents = contents1;
  ItemStack c2 = c1;
  CHECK(c1.same_item(c2));
  c2.potion_contents->custom_effects[0].duration = 200;
  CHECK(!c1.same_item(c2));
  // The display fields are components too: a tinted or renamed bottle is not
  // the same stack as the plain one.
  ItemStack tinted = healing_too;
  tinted.potion_contents->custom_color = 0x112233;
  CHECK(!tinted.same_item(healing_too));
  ItemStack named = healing_too;
  named.potion_contents->custom_name = "Bob";
  CHECK(!named.same_item(healing_too));

  // An emptied stack forgets its contents, which is what makes the glass bottle
  // the remainder leaves behind a plain one.
  healing.clear();
  CHECK(!healing.potion_contents.has_value());
}

// --- /give's argument form ---------------------------------------------------

void test_give_argument() {
  const int32_t potion_item = ItemRegistry::instance().id_of("minecraft:potion").value_or(-1);
  int32_t id = -1;
  std::optional<StackPotionContents> contents;
  std::string error;

  // The plain item token, with and without the namespace.
  CHECK(parse_item_with_potion("potion", &id, &contents, &error));
  CHECK_EQ(id, potion_item);
  CHECK(!contents.has_value());
  CHECK(parse_item_with_potion("minecraft:potion", &id, &contents, &error));
  CHECK(!contents.has_value());

  // The component, spelled the way vanilla spells it.
  CHECK(parse_item_with_potion("potion[potion_contents={potion:\"minecraft:healing\"}]", &id,
                               &contents, &error));
  CHECK_EQ(id, potion_item);
  CHECK(contents.has_value());
  if (contents)
    CHECK_EQ(contents->potion, 24);
  // The quotes are optional and the namespace may be left out, because both are
  // the same string to Brigadier's reader.
  CHECK(parse_item_with_potion("potion[potion_contents={potion:healing}]", &id, &contents, &error));
  CHECK(contents.has_value());
  if (contents)
    CHECK_EQ(contents->potion, 24);
  // A named potion with no effects is still a named potion: a water bottle is
  // present-but-empty, and that is not the same thing as no component at all.
  CHECK(parse_item_with_potion("potion[potion_contents={potion:\"minecraft:water\"}]", &id,
                               &contents, &error));
  CHECK(contents.has_value());
  if (contents) {
    CHECK_EQ(contents->potion, 0);
    CHECK(contents->custom_effects.empty());
  }
  // An empty field list names no potion, which is the component a bottle with
  // nothing in it carries.
  CHECK(parse_item_with_potion("potion[potion_contents={}]", &id, &contents, &error));
  CHECK(contents.has_value());
  if (contents) {
    CHECK_EQ(contents->potion, -1);
    CHECK(contents->empty());
  }

  // Everything refused, with the message the command shows.
  struct Refusal {
    const char* token;
    const char* message;
  };
  const Refusal refusals[] = {
      {"not_an_item", "Unknown item 'not_an_item'"},
      {"potion[foo=1]", "Unsupported component in 'potion[foo=1]'"},
      {"potion[potion_contents={potion:\"minecraft:nope\"}]", "Unknown potion"},
      {"potion[potion_contents={custom_color:1}]", "Unsupported potion_contents field"},
      {"potion[potion_contents={potion:\"minecraft:healing\",custom_name:\"x\"}]",
       "Unsupported potion_contents field"},
      {"potion[potion_contents=potion]", "Unsupported component in"},
      {"potion[potion_contents=", "Malformed item argument"},
      {"potion]", "Unknown item 'potion]'"},
  };
  for (const Refusal& refusal : refusals) {
    error.clear();
    CHECK(!parse_item_with_potion(refusal.token, &id, &contents, &error));
    if (error.find(refusal.message) == std::string::npos) {
      std::fprintf(stderr, "FAIL %s:%d: '%s' -> '%s', want '%s'\n", __FILE__, __LINE__,
                   refusal.token, error.c_str(), refusal.message);
      ++failures;
    }
    // A refused argument leaves nothing behind for the caller to use.
    CHECK(!contents.has_value());
  }
  // The normalizer rejects a name that is not an identifier at all.
  CHECK(!parse_item_with_potion("potion[potion_contents={potion:not a potion!}]", &id, &contents,
                                &error));
  CHECK(error.find("Unknown potion") != std::string::npos);
}

} // namespace

int main() {
  test_table();
  test_all_effects();
  test_instant_effects();
  test_lasting_effects();
  test_nothing_to_apply();
  test_write_component();
  test_read_component();
  test_same_item();
  test_give_argument();
  if (failures == 0)
    std::printf("potion: all checks passed\n");
  else
    std::printf("potion: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
