// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fire: the `minecraft:fire` block as a hazard, and the burning state of a
// living entity (Phase 3 milestone G follow-up).
//
// Before this, fire existed only as a *predicate* in the potion module: a water
// bottle knew a target could be "on fire" and that a block could be doused, but
// nothing could be on fire and nothing could burn. Both halves are real now,
// which is what makes `AbstractThrownPotion.onHitBlock` and `onHitAsWater`
// observable instead of unfalsifiable.
//
// Modelled, and from where:
//   * `FlintAndSteelItem.useOn` — light a campfire or a candle (setting `lit`),
//     or place fire in the cell past the clicked face when it is air and
//     `BaseFireBlock.canBePlacedAt` allows it; the item is *damaged*, not
//     consumed;
//   * `BaseFireBlock.getState` / `FireBlock.getStateForPlacement` — soul fire
//     over a soul-fire base block, else fire at age 0 whose five connection
//     properties say which neighbours burn;
//   * `FireBlock.canSurvive` — the block below is sturdy, or a neighbour burns.
//     `FireBlock.canBurn` reads the generated flammability table
//     (src/world/fire_table.h);
//   * `FireBlock.tick`, both halves of the table's use: the spread into a cell
//     whose neighbours are flammable (`(igniteOdds + 40 + difficulty * 7) /
//     (age + 30)` against a distance-scaled rate), the `nextInt(3) / 2` ageing
//     up to `MAX_AGE`, the age-15 dead-end, and the six-neighbour burn-out that
//     consumes what the fire is actually sitting on. The tick reschedules itself
//     through the block-tick loop (world/block_tick.h) — see `fire_tick` below;
//   * `BaseFireBlock.entityInside` — a living entity whose box overlaps a fire
//     block is ignited for 8 seconds (`fireIgnite`) and takes that block's own
//     damage (1.0 for fire, 2.0 for soul fire) on every tick it is inside;
//   * `Entity.baseTick` — the 1.0 `on_fire` damage every 20 ticks while
//     `remainingFireTicks` counts down, and the shared on-fire flag the client
//     draws the flames from; `Entity.extinguishFire`'s clear;
//   * `AbstractThrownPotion.dowseFire` and its caller — the six positions a
//     water bottle douses: the cell the bottle was in, the block it hit, and
//     that cell's four horizontal neighbours.
//
// What is NOT modelled, and why:
//   * **fire's rain branches need weather to be real.** `FireBlock.tick`'s two
//     rain branches (the `isRaining() && isNearRain(...)` removal at
//     `0.2f + age * 0.03f`, and the same gate on a spread) and `checkBurnOut`'s
//     `!isRainingAt(pos)` are all implemented and are asked of `FireLevel`'s
//     `is_raining` / `is_raining_at` (world/weather.h supplies both). A caller
//     that leaves them null gets "not raining", which is what a unit test
//     without weather wants. Note this also means rain is *per cell*: it takes
//     a biome that precipitates (world/biome_table.h) for fire to be rained out
//     at all — in the default in-memory world (`minecraft:the_void`) it never
//     is, and that is vanilla's own answer for that biome;
//   * **`canSpreadFireAround`'s `FIRE_SPREAD_RADIUS_AROUND_PLAYER` gamerule** is
//     always the -1 "no limit" value and `increased_fire_burnout` is always its
//     false default, so neither is threaded through;
//   * **the priming itself is the caller's.** `checkBurnOut`'s TNT branch is
//     real: the block it just consumed is handed to `FireLevel::prime_tnt`, and
//     the engine fills that hook (the session's `prime_tnt_block`,
//     world/tnt.h's rules) with a lit entity that has no owner — a fire is
//     nobody's flint and steel. A caller that leaves the hook null gets a world
//     with vanilla's *absence* of primeable TNT, where the block burns away;
//   * **a player is never fire-immune.** `Entity.fireImmune()` is a property of
//     the entity *type*, so the parameter is the caller's: the mob tick passes
//     the generated table's own `fire_immune` (a blaze, a magma cube, a strider
//     and the rest of vanilla's immune types), and the player paths pass false;
//     that flag is `Entity.isInvulnerableTo`'s `IS_FIRE` refusal as well — a
//     fire-immune mob is not ignited and not hurt by the flames;
//   * **the `burning_time` attribute** (`LivingEntity.igniteForTicks` scales the
//     ticks by it) is not applied: it is only ever modified by equipment this
//     server does not model, so it stays 1.0;
//   * **fire's portal branch.** `BaseFireBlock.getState` and `onPlace` look for
//     a portal shape in an obsidian frame; nothing here can build one, so an
//     obsidian frame lights no portal. `canBePlacedAt`'s own answer is
//     unaffected — `isPortal` only ever *widens* what is allowed, and a cell
//     beside obsidian with a sturdy or burning neighbour passes either way;
//   * **sounds and particles.** This server sends no sound packets at all, so
//     flint-and-steel's strike, a candle's extinguish and a player's own fire
//     break (`levelEvent 1009` from `playerWillDestroy`) are silent. The water
//     douse's 1009 for a campfire *is* sent, because that one is the potion's
//     own packet. Candle smoke particles are not spawned.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "world/chunk.h"

namespace mc::world {

// ---- vanilla constants ------------------------------------------------------

// `FireBlock.MAX_AGE`: the state's `age` ceiling, and the point the burn-out
// rule starts removing fire that has nothing left to feed on.
constexpr int kFireMaxAge = 15;
// `FireBlock.getFireTickDelay`: `30 + random.nextInt(10)`. A fire block ticks
// itself on this delay and reschedules at the top of every tick, which is the
// cadence the block-tick loop (world/block_tick.h) runs it at.
constexpr int kFireTickMinDelay = 30;
constexpr int kFireTickDelaySpread = 10;

// `FireBlock`'s and `SoulFireBlock`'s own `fireDamage` constructor arguments.
constexpr float kFireBlockDamage = 1.0f;
constexpr float kSoulFireBlockDamage = 2.0f;
// `BaseFireBlock.SECONDS_ON_FIRE`, the length of `fireIgnite`.
constexpr float kFireSecondsOnFire = 8.0f;
// `BaseFireBlock.MIN_FIRE_TICKS_TO_ADD` / `MAX_FIRE_TICKS_TO_ADD`: the
// `nextInt(min, max)` a *player* walking into fire has added, where a mob's
// burntime is only ever bumped by one.
constexpr int kFireMinTicksToAdd = 1;
constexpr int kFireMaxTicksToAdd = 3;
// `Player.getFireImmuneTicks`: 20 ticks, where every other entity here answers
// 0. It is the depth of the negative countdown a player idles at, and therefore
// the second of standing in fire they get before the flames catch.
constexpr int kPlayerFireImmuneTicks = 20;

// ---- names ------------------------------------------------------------------

constexpr std::string_view kFireBlock = "minecraft:fire";
constexpr std::string_view kSoulFireBlock = "minecraft:soul_fire";

// `#minecraft:soul_fire_base_blocks` (soul sand and soul soil): the block below
// that makes the fire above it *soul* fire.
bool fire_is_soul_base(const BlockStateKey& state);
// `BlockTags.FIRE`, i.e. `minecraft:fire` and `minecraft:soul_fire`.
bool fire_is_fire(const BlockStateKey& state);
// `Level.isEmptyBlock`: air, cave air or void air.
bool fire_is_air(const BlockStateKey& state);

// `FireBlock.getIgniteOdds` for a block *name*, 0 when the block is not in the
// table. `FireBlock.canBurn(state)` is this being non-zero, with a waterlogged
// state answering 0 whatever its block would say.
int fire_ignite_odds(std::string_view block_name);
bool fire_block_can_burn(const BlockStateKey& state);
// The table's other half: `FireBlock.getBurnOdds(state)` — how readily fire
// consumes the block once it is alight next to it. 0 for an unknown block and
// for a waterlogged state (`getBurnOdds`'s own `WATERLOGGED` check).
int fire_burn_odds(const BlockStateKey& state);
// `state.is(level.dimensionType().infiniburn())`: netherrack and magma block
// (`#minecraft:infiniburn_overworld`), the two blocks a fire standing on top of
// never ages out or spreads from. This server only has the overworld.
bool fire_is_infiniburn(const BlockStateKey& state);
// `state.getValue(FireBlock.AGE)` for a fire state, 0 for anything else or a
// state whose `age` property is unreadable.
int fire_age_of(const BlockStateKey& state);

// ---- the world the rules read -----------------------------------------------

// `is_sturdy` answers `BlockState.isFaceSturdy(level, pos, Direction.UP)`. This
// server has no collision shapes, so callers pass the same "not air and not a
// fluid" predicate the mobs and orbs collide with; an empty one means nothing is
// sturdy (which is what a bare unit test wants).
struct FireWorld {
  std::function<BlockStateKey(int, int, int)> get_block;
  std::function<bool(int, int, int)> is_sturdy;
};

// `FireBlock.getStateForPlacement` (via `BaseFireBlock.getState`): the state
// fire takes when it is placed at this cell. A name/properties key, so the
// caller looks its network id up in the block-state table like any placement.
BlockStateKey fire_placement_state(const FireWorld& world, int x, int y, int z);

// `FireBlock.canSurvive`: the block below is sturdy, or one of the six
// neighbours burns (`isValidFireLocation`).
bool fire_can_survive(const FireWorld& world, int x, int y, int z);

// `BaseFireBlock.canBePlacedAt`: the cell is empty and the state fire would take
// there can survive. The portal branch is the one documented omission.
bool fire_can_be_placed_at(const FireWorld& world, int x, int y, int z);

// ---- the block's own tick ----------------------------------------------------

// The world `FireBlock.tick` mutates. `get_block` and `is_sturdy` are the same
// read-only surface `FireWorld` exposes; `set_block` is `Level.setBlock` with its
// update flags reduced to the one thing fire's calls ever differ on — whether the
// *client* is told (`UPDATE_CLIENTS`). The age-only write in `tick` uses flags
// 260, whose `UPDATE_CLIENTS` bit is clear, so a fire ageing up is invisible on
// the wire; the spread, burn-out and removal writes all use flags 3 and are sent.
// `schedule_tick` is `Level.scheduleTick`, i.e. `BlockTickQueue::schedule`.
struct FireLevel : FireWorld {
  std::function<void(int, int, int, const BlockStateKey&, bool notify)> set_block;
  std::function<void(int, int, int, int delay)> schedule_tick;
  // `Level.isRaining()` and `Level.isRainingAt(pos)`: the two questions the rain
  // branches below ask. Both may be null (a test with no weather), and a null
  // one reads as "not raining" — the answer this module gave before the weather
  // existed. `is_raining_at` is the *per cell* one: it is true only where the
  // biome precipitates and nothing roofs the cell, so a fire in the nether, in
  // a cave, or in the `the_void` world is never rained out.
  std::function<bool()> is_raining;
  std::function<bool(int, int, int)> is_raining_at;
  // `TntBlock.prime(level, pos)`: the block `checkBurnOut` just consumed — or
  // replaced with fire — was TNT, so it becomes a primed entity instead of
  // simply vanishing. Called with vanilla's argument (the position, and no
  // source: a fire is nobody's flint and steel). A null hook leaves the TNT to
  // burn away through the generic branch, which is what a test without a TNT
  // registry wants.
  std::function<void(int, int, int)> prime_tnt;
};

// The draws `FireBlock.tick` takes, in call order: `next_int(bound)` is
// `RandomSource.nextInt(bound)`, uniform in [0, bound), and `next_float()` is
// `RandomSource.nextFloat()` (the rain douse's `0.2f + age * 0.03f` chance).
// Passed in (rather than owned) so a test can script the tick's every branch.
struct FireRandom {
  std::function<int(int bound)> next_int;
  std::function<float()> next_float;
};

// `FireBlock.tick`, the whole of it.
//
// It reschedules itself first (so a surviving fire keeps ticking), removes itself
// when `canSurvive` fails, and then — unless it is standing on an infiniburn
// block — runs vanilla's three rain branches: the douse (raining, a rained-on
// neighbour, and a `0.2 + age * 0.03` float draw, which removes the block and
// returns before the ageing), the same `isNearRain` gate on each spread, and
// `checkBurnOut`'s choice to remove the block rather than leave the fire behind
// when it is raining on it. Otherwise it ages by `nextInt(3) / 2` up to
// `MAX_AGE` and checks six neighbours for burn-out plus its whole
// `-1..1 x -1..1 z -1..4` box for a spread. Water-logged and unknown blocks
// contribute nothing.
//
// `difficulty` is `level.getDifficulty().getId()` (0..3) and only scales the
// spread odds. The caller provides it; this server has a single difficulty.
void fire_tick(FireLevel& level, const FireRandom& rng, int difficulty, int x, int y, int z);

// ---- the water bottle's douse ----------------------------------------------

struct FireBlockPos {
  int x = 0, y = 0, z = 0;
};

// The step of one `Direction` ordinal as the wire numbers them: 0 down, 1 up,
// 2 north, 3 south, 4 west, 5 east.
void block_face_step(int face, int* dx, int* dy, int* dz);

// `AbstractThrownPotion.onHitBlock`'s four-way douse, for a bottle that broke on
// the `face` of the block at (hit_x, hit_y, hit_z): the cell the bottle was in
// (`blockEffectPos = blockHitPos.relative(hitDirection)`), the block it hit
// (that cell's opposite neighbour), and that cell's four horizontal neighbours —
// six positions, in vanilla's own order.
std::vector<FireBlockPos> fire_douse_positions(int hit_x, int hit_y, int hit_z, int face);

// What a douse does to one position.
enum class DouseKind : uint8_t {
  kNone = 0, // nothing here burns
  kFire,     // a fire block: `destroyBlock(pos, false)`
  kCampfire, // a lit campfire: level event 1009, then `lit=false`
  kCandle,   // a lit candle or candle cake: `lit=false`
};

// `blockState.is(BlockTags.FIRE)`, `CampfireBlock.isLitCampfire` and
// `AbstractCandleBlock.isLit`, in that order (which is vanilla's own order of
// tests). The `#minecraft:campfires`, `#minecraft:candles` and
// `#minecraft:candle_cakes` memberships are the two campfire names and the
// `_candle` / `_candle_cake` suffixes, which is exactly what those tags hold in
// 26.2.
DouseKind fire_douse_kind(const BlockStateKey& state);

// The state of a doused campfire or candle: `lit=false`, every other property
// kept (`state.setValue(LIT, false)`).
BlockStateKey fire_doused_state(const BlockStateKey& state);

// `CampfireBlock.canLight` / `CandleBlock.canLight` / `CandleCakeBlock.canLight`:
// whether flint and steel can light this block (so `useOn` applies
// `state.setValue(LIT, true)` instead of placing fire).
bool fire_can_light(const BlockStateKey& state);
// That state: `lit=true`, every other property kept.
BlockStateKey fire_lit_state(const BlockStateKey& state);

// ---- the burning entity -----------------------------------------------------

// `Entity.remainingFireTicks` and the four things vanilla reads it for. Shared
// by the player's SurvivalState and by a mob, because `baseTick`, the block pass
// and `fireIgnite` are the same code for both.
//
// The countdown is *signed* and that is the whole trick: `applyEffectsFromBlocks`
// leaves it at `-getFireImmuneTicks()` whenever the entity is not burning, and
// `fireIgnite` walks it back up by one a tick, so a player (whose immunity is 20
// ticks, unlike every mob's 0) takes a full second of standing in a fire before
// they catch. The caller drives the three steps in vanilla's order:
//
//     if (burning.tick(immune, in_lava))                     // baseTick
//       hurt("on_fire", 1.0f);
//     float fire_damage = 0.0f;
//     bool ignited = false;
//     if (fire_entity_inside(world, box, &fire_damage)) {     // applyEffectsFromBlocks
//       ignited = fire_ignite(burning, immune, is_player, roll);
//       hurt("in_fire", fire_damage);
//     }
//     burning.reset_if_not_burning(ignited);
struct Burning {
  int remaining_ticks = 0;
  // `Entity.getFireImmuneTicks`: 20 for a player, 0 for everything else this
  // server can spawn. The idle countdown is the negative of this.
  int immune_ticks = 0;

  // `Entity.isOnFire`: `!fireImmune() && remainingFireTicks > 0`. The client
  // draws the flames from the shared flag, which is this value.
  bool on_fire() const {
    return remaining_ticks > 0;
  }

  // `Entity.igniteForTicks`: only ever *raises* the countdown.
  void ignite_ticks(int ticks) {
    if (remaining_ticks < ticks)
      remaining_ticks = ticks;
  }
  // `Entity.igniteForSeconds`: `floor(seconds * 20)`, which for the 8 seconds
  // fire ignites for is 160 ticks.
  void ignite_seconds(float seconds) {
    ignite_ticks(static_cast<int>(seconds * 20.0f));
  }
  // `Entity.clearFire`: `setRemainingFireTicks(min(0, getRemainingFireTicks()))`
  // — a negative countdown (which is what gates `fireIgnite`'s bump) is kept.
  void clear() {
    if (remaining_ticks > 0)
      remaining_ticks = 0;
  }
  // `Entity.extinguishFire`: the clear, plus a sound the caller would send.
  void extinguish() {
    clear();
  }

  // `Entity.baseTick`'s fire block. Returns true when this tick's 1.0
  // `minecraft:on_fire` damage is due; a fire-immune entity clears instead and
  // never counts down. `in_lava` suppresses the damage but not the countdown,
  // exactly as vanilla's `remainingFireTicks % 20 == 0 && !this.isInLava()`.
  bool tick(bool fire_immune, bool in_lava = false) {
    if (remaining_ticks <= 0)
      return false;
    if (fire_immune) {
      clear();
      return false;
    }
    const bool hurt = remaining_ticks % 20 == 0 && !in_lava;
    --remaining_ticks;
    return hurt;
  }

  // `applyEffectsFromBlocks`'s tail: an entity standing in rain is put out
  // first (`if (this.isInRain()) this.clearFire()`), and then an entity that is
  // not on fire and was not ignited by this tick's block pass has its countdown
  // reset to `-getFireImmuneTicks()`. `in_rain` is `Entity.isInRain()`:
  // `isRainingAt(feet)` or `isRainingAt(box top)`, so a player under a roof is
  // not put out by the storm above it.
  void reset_if_not_burning(bool ignited_this_tick, bool in_rain = false) {
    if (in_rain)
      clear();
    if (!on_fire() && !ignited_this_tick)
      remaining_ticks = -immune_ticks;
  }
};

// A fresh countdown at the right idle depth for its owner: 20 ticks of immunity
// for a player, none for a mob.
inline Burning player_burning() {
  Burning burning;
  burning.immune_ticks = kPlayerFireImmuneTicks;
  return burning;
}

// `BaseFireBlock.fireIgnite`: walking into fire. A countdown below zero is
// nudged up by one; a *player* at or above zero has `nextInt(1, 3)` added (1 or
// 2), which is what `added_ticks` carries — pass a draw in [1, 3). Once the
// countdown is at or above zero the entity ignites for 8 seconds. Fire-immune
// entities are untouched. Returns whether the countdown grew, which is vanilla's
// `wasIgnitedThisTick` and what suppresses the reset above.
bool fire_ignite(Burning& burning, bool fire_immune, bool is_player, int added_ticks);

// `BaseFireBlock.entityInside`'s half this server can see: is any cell the box
// overlaps a fire block, and what damage does it deal? `*out_damage` receives
// the *last* fire block found (vanilla applies the effect once per overlapping
// block, so soul fire inside a fire cluster wins only if it is the one the box
// was checked through last — in practice an entity stands in one block).
bool fire_entity_inside(const FireWorld& world, double min_x, double min_y, double min_z,
                        double max_x, double max_y, double max_z, float* out_damage);

} // namespace mc::world
