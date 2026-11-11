// SPDX-License-Identifier: GPL-3.0-or-later
//
// `minecraft:consumable`'s `on_consume_effects` list (Phase 3 milestone G
// follow-up). See world/consume.h for what is modelled and where the boundary
// sits.
#include "world/consume.h"

#include <algorithm>
#include <cmath>

namespace mc::world {
namespace {

// The hair every box query in this server shrinks by (the same one
// `mob_box_free` uses): an entity resting exactly on a block's top face must not
// count the floor it stands on as a collision, and BlockCollisions shrinks the
// query box the same way before it walks the blocks it overlaps.
constexpr double kBoxEpsilon = 1.0e-7;

// Whether any block overlapping the entity's box satisfies `test`.
bool box_hits(const std::function<bool(int, int, int)>& test, double x, double y, double z,
              double half_width, double height) {
  const int x0 = static_cast<int>(std::floor(x - half_width + kBoxEpsilon));
  const int x1 = static_cast<int>(std::floor(x + half_width - kBoxEpsilon));
  const int y0 = static_cast<int>(std::floor(y + kBoxEpsilon));
  const int y1 = static_cast<int>(std::floor(y + height - kBoxEpsilon));
  const int z0 = static_cast<int>(std::floor(z - half_width + kBoxEpsilon));
  const int z1 = static_cast<int>(std::floor(z + half_width - kBoxEpsilon));
  for (int bx = x0; bx <= x1; ++bx)
    for (int by = y0; by <= y1; ++by)
      for (int bz = z0; bz <= z1; ++bz)
        if (test(bx, by, bz))
          return true;
  return false;
}

} // namespace

int consume_effects_for_item(int32_t item, const GeneratedConsumeEffect** first) {
  if (first)
    *first = nullptr;
  for (int i = 0; i < kConsumeEffectCount; ++i) {
    if (kConsumeEffects[i].item != item)
      continue;
    int count = 1;
    while (i + count < kConsumeEffectCount && kConsumeEffects[i + count].item == item)
      ++count;
    if (first)
      *first = &kConsumeEffects[i];
    return count;
  }
  return 0;
}

bool consume_effect_fires(const GeneratedConsumeEffect& effect, double unit) {
  // The comparison is float-against-float, because `RandomSource.nextFloat()`
  // returns a float and the probability is one too: 0.3 is 0.30000001192 and a
  // *double* 0.3 would compare below it, turning `>= probability` (a refusal)
  // into a fire. Narrowing the caller's value is what makes a unit of exactly
  // 0.3 behave like a nextFloat of 0.3.
  return static_cast<float>(unit) < effect.value;
}

bool apply_consume_status_effects(const GeneratedConsumeEffect& effect, double unit,
                                  ActiveEffects& effects, EffectOutcome& out) {
  if (effect.kind != kConsumeKindApplyEffects || !consume_effect_fires(effect, unit))
    return false;
  // A row that pointed outside the entry array would be a generator bug; fail
  // closed rather than read past it.
  if (effect.first < 0 || effect.count < 0 ||
      effect.first + effect.count > kConsumeStatusEffectCount)
    return false;
  bool any_applied = false;
  for (int i = 0; i < effect.count; ++i) {
    const GeneratedConsumeStatusEffect& row = kConsumeStatusEffects[effect.first + i];
    MobEffectInstance instance;
    instance.effect = row.effect;
    instance.duration = row.duration;
    instance.amplifier = row.amplifier;
    instance.ambient = row.ambient;
    instance.visible = row.visible;
    instance.show_icon = row.show_icon;
    if (effects.add(instance, out))
      any_applied = true;
  }
  return any_applied;
}

bool remove_consume_status_effects(const GeneratedConsumeEffect& effect, ActiveEffects& effects,
                                   std::vector<int32_t>* removed) {
  if (effect.kind != kConsumeKindRemoveEffects)
    return false;
  if (effect.first < 0 || effect.count < 0 ||
      effect.first + effect.count > kConsumeRemoveEffectCount)
    return false;
  bool any_removed = false;
  for (int i = 0; i < effect.count; ++i) {
    const int32_t id = kConsumeRemoveEffects[effect.first + i];
    if (!effects.remove(id))
      continue;
    if (removed)
      removed->push_back(id);
    any_removed = true;
  }
  return any_removed;
}

bool clear_all_consume_effects(ActiveEffects& effects, std::vector<int32_t>* removed) {
  if (effects.empty())
    return false;
  if (removed)
    for (const MobEffectInstance& instance : effects.all())
      removed->push_back(instance.effect);
  return effects.remove_all();
}

bool random_teleport(const RandomTeleportQuery& query, double x, double y, double z,
                     double half_width, double height, RandomTeleportTarget* out) {
  if (!query.blocks_motion || !query.liquid || !out)
    return false;
  // `BlockPos.containing(xx, yy, zz)`: the search starts in the candidate's own
  // block and never moves horizontally, so its x and z are fixed for the walk.
  const int bx = static_cast<int>(std::floor(x));
  const int bz = static_cast<int>(std::floor(z));
  double landed_y = y;
  int by = static_cast<int>(std::floor(y));
  bool landed = false;
  while (!landed && by > query.min_y) {
    if (query.blocks_motion(bx, by - 1, bz)) {
      landed = true;
      break;
    }
    landed_y -= 1.0;
    --by;
  }
  if (!landed)
    return false; // nothing to stand on before the bottom of the world
  // `level.noCollision(this) && !level.containsAnyLiquid(this.getBoundingBox())`:
  // both are box queries against the landing position.
  if (box_hits(query.blocks_motion, x, landed_y, z, half_width, height) ||
      box_hits(query.liquid, x, landed_y, z, half_width, height))
    return false;
  out->x = x;
  out->y = landed_y;
  out->z = z;
  return true;
}

} // namespace mc::world
