// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for `minecraft:consumable`'s `on_consume_effects` list (the
// milestone G follow-up):
//   * the generated table: eleven items, all five kinds, the item ids it keys on
//     are the item registry's own, and one item's rows are adjacent — which is
//     what `consume_effects_for_item`'s walk relies on;
//   * every effect id the table names resolves in the mob_effect registry,
//     because an id that does not is a consume that silently does nothing;
//   * the golden apple's two entries exactly as the data has them (Regeneration
//     II for 100 ticks, Absorption I for 2400), the order they apply in and the
//     `onEffectStarted` side effect the first one produces;
//   * ApplyStatusEffectsConsumeEffect's probability gate at its boundary:
//     `nextFloat() >= probability` refuses, so 0.3 fires at 0.2999 and not at
//     0.3, and a refused roll changes nothing at all;
//   * the removal kind (honey bottle: poison only) and the clear-all kind (milk
//     bucket: everything, reported before the set empties);
//   * LivingEntity.randomTeleport's landing search: the descent, the "nothing to
//     stand on" refusal at the bottom of the world, and its two collision gates.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/consume.h"
#include "world/effect.h"
#include "world/item.h"
#include "world/item_registry.h"

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

int32_t id_of(const char* name) {
  const GeneratedMobEffect* def = mob_effect_by_name(name);
  return def ? mob_effect_id_of(*def) : -1;
}

int32_t item_of(const char* name) {
  return ItemRegistry::instance().id_of(name).value_or(-1);
}

// The rows for `name`, as pointers into the generated table (so nothing here
// ever refers to a temporary).
std::vector<const GeneratedConsumeEffect*> rows_for(const char* name) {
  const GeneratedConsumeEffect* rows = nullptr;
  const int count = consume_effects_for_item(item_of(name), &rows);
  std::vector<const GeneratedConsumeEffect*> out;
  for (int i = 0; i < count; ++i)
    out.push_back(&rows[i]);
  return out;
}

const GeneratedConsumeEffect* row_of_kind(const char* name, uint8_t kind) {
  for (const GeneratedConsumeEffect* row : rows_for(name))
    if (row->kind == kind)
      return row;
  return nullptr;
}

std::string number(float value) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(value));
  return buf;
}

std::string describe(const EffectOutcome& outcome) {
  std::string out;
  for (int32_t id : outcome.added)
    out += "add:" + std::to_string(id) + " ";
  for (int32_t id : outcome.updated)
    out += "upd:" + std::to_string(id) + " ";
  for (int32_t id : outcome.removed)
    out += "rm:" + std::to_string(id) + " ";
  for (const EffectEvent& event : outcome.events)
    out +=
        "event(" + std::to_string(static_cast<int>(event.kind)) + ":" + number(event.amount) + ") ";
  return out;
}

// --- the table ---------------------------------------------------------------

void test_table() {
  // Eleven items carry the list, and all five kinds are represented: a kind that
  // vanished from the data would otherwise be invisible here.
  int kinds[kConsumeKindCount] = {0};
  int items = 0;
  int32_t last_item = -1;
  for (int i = 0; i < kConsumeEffectCount; ++i) {
    const GeneratedConsumeEffect& row = kConsumeEffects[i];
    CHECK(row.kind < kConsumeKindCount);
    ++kinds[row.kind];
    // Sorted by item id, with one item's rows adjacent.
    CHECK(row.item >= last_item);
    if (row.item != last_item) {
      ++items;
      last_item = row.item;
    }
    // The ids are the item registry's, so a row cannot point at an item whose
    // effects it is not.
    CHECK(ItemRegistry::instance().by_id(row.item) != nullptr);
  }
  CHECK(items == 11);
  CHECK(kConsumeEffectCount == 11);
  CHECK(kinds[kConsumeKindApplyEffects] == 7);
  CHECK(kinds[kConsumeKindRemoveEffects] == 1);
  CHECK(kinds[kConsumeKindClearAllEffects] == 1);
  CHECK(kinds[kConsumeKindTeleportRandomly] == 1);
  CHECK(kinds[kConsumeKindPlaySound] == 1);

  // Every effect the table names has to exist in the registry.
  for (int i = 0; i < kConsumeStatusEffectCount; ++i) {
    const GeneratedConsumeStatusEffect& row = kConsumeStatusEffects[i];
    CHECK(mob_effect_by_id(row.effect) != nullptr);
    CHECK(row.amplifier >= 0 && row.amplifier <= kMaxEffectAmplifier);
  }
  for (int i = 0; i < kConsumeRemoveEffectCount; ++i)
    CHECK(mob_effect_by_id(kConsumeRemoveEffects[i]) != nullptr);

  // An apply row's span stays inside the status array and a remove row's inside
  // the removals array — the two bounds the appliers check before they walk.
  for (int i = 0; i < kConsumeEffectCount; ++i) {
    const GeneratedConsumeEffect& row = kConsumeEffects[i];
    CHECK(row.first >= 0 && row.count >= 0);
    if (row.kind == kConsumeKindApplyEffects)
      CHECK(row.first + row.count <= kConsumeStatusEffectCount);
    else if (row.kind == kConsumeKindRemoveEffects)
      CHECK(row.first + row.count <= kConsumeRemoveEffectCount);
    else
      CHECK(row.count == 0);
  }

  // An item with no list is empty, not "the first item's list".
  const GeneratedConsumeEffect* rows = nullptr;
  CHECK(consume_effects_for_item(item_of("minecraft:apple"), &rows) == 0);
  CHECK(rows == nullptr);
  CHECK(consume_effects_for_item(item_of("minecraft:cod_bucket"), &rows) == 0);
  CHECK(consume_effects_for_item(item_of("minecraft:cooked_beef"), &rows) == 0);
}

void test_item_data() {
  std::vector<const GeneratedConsumeEffect*> rows = rows_for("minecraft:golden_apple");
  CHECK(rows.size() == 1);
  CHECK(rows[0]->kind == kConsumeKindApplyEffects);
  CHECK_NEAR(rows[0]->value, 1.0, 1e-6); // no probability gate on an apple
  CHECK(rows[0]->count == 2);

  const GeneratedConsumeStatusEffect& regen = kConsumeStatusEffects[rows[0]->first];
  CHECK(regen.effect == id_of("minecraft:regeneration"));
  CHECK(regen.duration == 100);
  CHECK(regen.amplifier == 1); // Regeneration II
  CHECK(regen.visible && regen.show_icon);

  const GeneratedConsumeStatusEffect& absorption = kConsumeStatusEffects[rows[0]->first + 1];
  CHECK(absorption.effect == id_of("minecraft:absorption"));
  CHECK(absorption.duration == 2400);
  CHECK(absorption.amplifier == 0);

  // The enchanted apple is the four-entry list: Regeneration II, Resistance I,
  // Fire Resistance I and Absorption IV.
  rows = rows_for("minecraft:enchanted_golden_apple");
  CHECK(rows.size() == 1);
  CHECK(rows[0]->count == 4);
  CHECK(kConsumeStatusEffects[rows[0]->first + 3].effect == id_of("minecraft:absorption"));
  CHECK(kConsumeStatusEffects[rows[0]->first + 3].amplifier == 3);

  // The one item whose row is really about the probability: a chicken's hunger
  // only lands 30% of the time.
  rows = rows_for("minecraft:chicken");
  CHECK(rows.size() == 1);
  CHECK_NEAR(rows[0]->value, 0.3, 1e-6);
  CHECK(kConsumeStatusEffects[rows[0]->first].effect == id_of("minecraft:hunger"));
  CHECK(kConsumeStatusEffects[rows[0]->first].duration == 600);
}

// --- the probability gate ----------------------------------------------------

void test_probability_gate() {
  const GeneratedConsumeEffect* chicken = rows_for("minecraft:chicken")[0];
  CHECK(!consume_effect_fires(*chicken, 0.3)); // `nextFloat() >= probability`
  CHECK(!consume_effect_fires(*chicken, 0.5));
  CHECK(!consume_effect_fires(*chicken, 0.999));
  CHECK(consume_effect_fires(*chicken, 0.2999));
  CHECK(consume_effect_fires(*chicken, 0.0));

  // A probability of 1 refuses nothing nextFloat can produce.
  const GeneratedConsumeEffect* apple = rows_for("minecraft:golden_apple")[0];
  CHECK(consume_effect_fires(*apple, 0.0));
  CHECK(consume_effect_fires(*apple, 0.9999999));

  // A refused roll changes nothing at all: not a short instance, not one entry.
  ActiveEffects effects;
  EffectOutcome outcome;
  CHECK(!apply_consume_status_effects(*chicken, 0.3, effects, outcome));
  CHECK(effects.size() == 0);
  CHECK(describe(outcome).empty());

  // ...and the same row at 0.2999 lands Hunger, and nothing else.
  CHECK(apply_consume_status_effects(*chicken, 0.2999, effects, outcome));
  CHECK(effects.size() == 1);
  const MobEffectInstance* hunger = effects.get(id_of("minecraft:hunger"));
  CHECK(hunger != nullptr);
  CHECK(hunger && hunger->duration == 600 && hunger->amplifier == 0);
  CHECK(outcome.added == std::vector<int32_t>{id_of("minecraft:hunger")});
}

// --- applying, removing, clearing --------------------------------------------

void test_apply_golden_apple() {
  ActiveEffects effects;
  EffectOutcome outcome;
  const GeneratedConsumeEffect* apple = rows_for("minecraft:golden_apple")[0];
  CHECK(apply_consume_status_effects(*apple, 0.0, effects, outcome));
  // Both landed, in the order the item lists them, and Absorption's
  // onEffectStarted reported itself: max(current, 4 * (1 + amplifier)) = 4.
  CHECK(describe(outcome) == "add:9 add:21 event(4:4) ");
  CHECK(effects.size() == 2);
  const MobEffectInstance* regen = effects.get(id_of("minecraft:regeneration"));
  const MobEffectInstance* absorption = effects.get(id_of("minecraft:absorption"));
  CHECK(regen && regen->duration == 100 && regen->amplifier == 1);
  CHECK(absorption && absorption->duration == 2400 && absorption->amplifier == 0);
  CHECK(outcome.events.size() == 1);
  CHECK(outcome.events[0].kind == EffectEventKind::kAbsorb);
  CHECK_NEAR(outcome.events[0].amount, 4.0, 1e-6);

  // The instances are copies of the row, so a second apple grants the same
  // instance rather than a mutated one — and nothing about it changes, because
  // MobEffectInstance.update only ever strengthens and equal is not stronger.
  EffectOutcome again;
  CHECK(!apply_consume_status_effects(*apple, 0.0, effects, again));
  CHECK(describe(again) == "event(4:4) ");
  CHECK(effects.get(id_of("minecraft:regeneration"))->duration == 100);
  CHECK(effects.get(id_of("minecraft:absorption"))->duration == 2400);

  // A stronger instance already running refuses the apple's weaker one, and the
  // absorption event still fires: vanilla calls onEffectStarted on the new
  // instance unconditionally (LivingEntity.addEffect's last line).
  MobEffectInstance strong;
  strong.effect = id_of("minecraft:regeneration");
  strong.duration = 400;
  strong.amplifier = 3;
  EffectOutcome strong_outcome;
  CHECK(effects.add(strong, strong_outcome));
  EffectOutcome refused;
  apply_consume_status_effects(*apple, 0.0, effects, refused);
  CHECK(describe(refused) == "event(4:4) ");
  CHECK(effects.get(id_of("minecraft:regeneration"))->amplifier == 3);
}

void test_milk_bucket_clears() {
  ActiveEffects effects;
  EffectOutcome seeded;
  MobEffectInstance speed;
  speed.effect = id_of("minecraft:speed");
  speed.duration = 600;
  MobEffectInstance poison;
  poison.effect = id_of("minecraft:poison");
  poison.duration = 100;
  effects.add(speed, seeded);
  effects.add(poison, seeded);

  const GeneratedConsumeEffect* row =
      row_of_kind("minecraft:milk_bucket", kConsumeKindClearAllEffects);
  CHECK(row != nullptr);
  CHECK(row && row->count == 0); // the kind carries no list of its own

  std::vector<int32_t> removed;
  CHECK(clear_all_consume_effects(effects, &removed));
  CHECK(removed == (std::vector<int32_t>{id_of("minecraft:speed"), id_of("minecraft:poison")}));
  CHECK(effects.empty());
  // Nothing left to clear.
  removed.clear();
  CHECK(!clear_all_consume_effects(effects, &removed));
  CHECK(removed.empty());
}

void test_honey_bottle_removes() {
  ActiveEffects effects;
  EffectOutcome seeded;
  MobEffectInstance poison;
  poison.effect = id_of("minecraft:poison");
  poison.duration = 100;
  MobEffectInstance hunger;
  hunger.effect = id_of("minecraft:hunger");
  hunger.duration = 600;
  effects.add(poison, seeded);
  effects.add(hunger, seeded);

  const GeneratedConsumeEffect* row =
      row_of_kind("minecraft:honey_bottle", kConsumeKindRemoveEffects);
  CHECK(row != nullptr);
  CHECK(row && row->count == 1);
  CHECK(row && kConsumeRemoveEffects[row->first] == id_of("minecraft:poison"));

  std::vector<int32_t> removed;
  CHECK(remove_consume_status_effects(*row, effects, &removed));
  CHECK(removed == std::vector<int32_t>{id_of("minecraft:poison")});
  CHECK(!effects.has(id_of("minecraft:poison")));
  CHECK(effects.has(id_of("minecraft:hunger"))); // nothing else is touched

  // Honey is not a poison cure when there is no poison to cure.
  removed.clear();
  CHECK(!remove_consume_status_effects(*row, effects, &removed));
  CHECK(removed.empty());
  CHECK(effects.size() == 1);
}

// --- LivingEntity.randomTeleport ---------------------------------------------

RandomTeleportQuery floor_at_64() {
  RandomTeleportQuery query;
  query.blocks_motion = [](int, int y, int) { return y <= 64; };
  query.liquid = [](int, int, int) { return false; };
  query.min_y = -64;
  return query;
}

void test_random_teleport() {
  const RandomTeleportQuery query = floor_at_64();
  RandomTeleportTarget target;

  // A candidate 4.5 above the surface lands 0.5 above it: the walk drops one
  // block at a time and keeps the candidate's fraction, so vanilla's landing y
  // is "the candidate less the blocks it descended", not a snapped surface (the
  // entity falls the last half block under its own gravity).
  CHECK(random_teleport(query, 0.5, 68.5, 0.5, 0.3, 1.8, &target));
  CHECK_NEAR(target.x, 0.5, 1e-9);
  CHECK_NEAR(target.y, 65.5, 1e-9);
  CHECK_NEAR(target.z, 0.5, 1e-9);

  // Already just above the floor: the block below it blocks motion, so the
  // position does not move at all.
  CHECK(random_teleport(query, 1.5, 65.5, -3.5, 0.3, 1.8, &target));
  CHECK_NEAR(target.y, 65.5, 1e-9);

  // A candidate whose *feet* are inside the floor (y=64.5 overlaps the block at
  // y=64) has a floor under it but no room in the box, so the second gate
  // refuses it — the `noCollision` check vanilla would fail there.
  CHECK(!random_teleport(query, 0.5, 64.5, 0.5, 0.3, 1.8, &target));

  // Nothing below the candidate anywhere: the descent runs out at getMinY and
  // the teleport is refused.
  CHECK(!random_teleport(query, 0.5, 40.0, 0.5, 0.3, 1.8, &target));
  // A candidate whose own block is at the bottom of the search space refuses
  // immediately (`while (pos.getY() > level.getMinY())`).
  CHECK(!random_teleport(query, 0.5, -64.0, 0.5, 0.3, 1.8, &target));

  // The second gate, and the reason it is a *box* query rather than another
  // column of the descent: a candidate 0.2 from a block boundary has a 0.6-wide
  // box that reaches into the neighbouring cell, so a solid block there refuses
  // the landing without ever being under the entity's own column.
  RandomTeleportQuery blocked = floor_at_64();
  blocked.blocks_motion = [](int x, int y, int z) {
    return y <= 64 || (x == -1 && y == 66 && z == 0);
  };
  CHECK(!random_teleport(blocked, 0.2, 68.5, 0.5, 0.3, 1.8, &target));
  // ...and a block one cell further out is outside the box, so it does not.
  RandomTeleportQuery beside = floor_at_64();
  beside.blocks_motion = [](int x, int y, int z) {
    return y <= 64 || (x == 1 && y == 66 && z == 0);
  };
  CHECK(random_teleport(beside, 0.5, 68.5, 0.5, 0.3, 1.8, &target));

  // The third gate: fluid anywhere in the landing box refuses it.
  RandomTeleportQuery flooded = floor_at_64();
  flooded.liquid = [](int, int y, int) { return y == 66; };
  CHECK(!random_teleport(flooded, 0.5, 68.5, 0.5, 0.3, 1.8, &target));
}

} // namespace

int main() {
  test_table();
  test_item_data();
  test_probability_gate();
  test_apply_golden_apple();
  test_milk_bucket_clears();
  test_honey_bottle_removes();
  test_random_teleport();
  if (failures == 0)
    std::printf("consume: all checks passed\n");
  else
    std::printf("consume: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
