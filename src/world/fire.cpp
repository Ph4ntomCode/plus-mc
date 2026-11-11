// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fire: the placement and survival rules of `minecraft:fire`, the burning state
// of a living entity, and the positions a water bottle douses. See
// world/fire.h for what is modelled and what is not.
#include "world/fire.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "world/container.h"
#include "world/fire_table.h"

namespace mc::world {
namespace {

constexpr double kCellEpsilon = 1.0E-7; // `BlockPos.containing(box.max - 1.0E-7)`

// The generated table is sorted by name, so both odds columns come out of one
// binary search.
const FireFlammability* find_flammability(std::string_view name) {
  const auto begin = std::begin(kFireFlammability);
  const auto end = std::end(kFireFlammability);
  const auto it =
      std::lower_bound(begin, end, name, [](const FireFlammability& row, std::string_view wanted) {
        return std::string_view(row.name) < wanted;
      });
  if (it == end || std::string_view(it->name) != name)
    return nullptr;
  return it;
}

// `BlockStateProperties.WATERLOGGED` is the one property both odds lookups
// refuse: a waterlogged block feeds no fire and burns as nothing.
bool is_waterlogged(const BlockStateKey& state) {
  return block_property(state.properties, "waterlogged") == "true";
}

// `FireBlock.getIgniteOdds(BlockState)` / `getBurnOdds(BlockState)`.
int state_ignite_odds(const BlockStateKey& state) {
  if (is_waterlogged(state))
    return 0;
  const FireFlammability* row = find_flammability(state.name);
  return row ? row->ignite_odds : 0;
}
int state_burn_odds(const BlockStateKey& state) {
  if (is_waterlogged(state))
    return 0;
  const FireFlammability* row = find_flammability(state.name);
  return row ? row->burn_odds : 0;
}

std::string prop(const BlockStateKey& state, std::string_view key) {
  return block_property(state.properties, key);
}

bool is_lit(const BlockStateKey& state) {
  return prop(state, "lit") == "true";
}

// `#minecraft:campfires` and `#minecraft:candles` / `#minecraft:candle_cakes`,
// whose 26.2 membership these name rules reproduce exactly.
bool is_campfire(std::string_view name) {
  return name == "minecraft:campfire" || name == "minecraft:soul_campfire";
}

// 17 each: the bare block plus the sixteen dyed ones. The bare name has no
// prefix before it (`minecraft:candle` does not end with `_candle`), so it needs
// naming on its own.
bool is_candle(std::string_view name) {
  return name == "minecraft:candle" || name.ends_with("_candle");
}

bool is_candle_cake(std::string_view name) {
  return name == "minecraft:candle_cake" || name.ends_with("_candle_cake");
}

bool can_burn_cell(const FireWorld& world, int x, int y, int z) {
  if (!world.get_block)
    return false;
  return fire_block_can_burn(world.get_block(x, y, z));
}

bool is_sturdy(const FireWorld& world, int x, int y, int z) {
  return world.is_sturdy && world.is_sturdy(x, y, z);
}

// `FireBlock.isValidFireLocation`: any of the six neighbours burns.
bool is_valid_fire_location(const FireWorld& world, int x, int y, int z) {
  static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                  {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
  for (const auto& step : kStep)
    if (can_burn_cell(world, x + step[0], y + step[1], z + step[2]))
      return true;
  return false;
}

// `FireBlock.getIgniteOdds(LevelReader, BlockPos)`: an *empty* cell takes the
// greatest of its six neighbours' ignite odds; anything else is 0.
int cell_ignite_odds(const FireWorld& world, int x, int y, int z) {
  if (!world.get_block || !fire_is_air(world.get_block(x, y, z)))
    return 0;
  static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                  {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
  int odds = 0;
  for (const auto& step : kStep)
    odds =
        std::max(odds, state_ignite_odds(world.get_block(x + step[0], y + step[1], z + step[2])));
  return odds;
}

// `Level.isRaining()`: a null source (a test without weather) never rains.
bool level_is_raining(const FireLevel& level) {
  return level.is_raining && level.is_raining();
}

// `Level.isRainingAt(pos)`, the per-cell question: null reads as false.
bool level_is_raining_at(const FireLevel& level, int x, int y, int z) {
  return level.is_raining_at && level.is_raining_at(x, y, z);
}

// `FireBlock.isNearRain`: the cell itself or one of its four horizontal
// neighbours, in vanilla's own order (west, east, north, south).
bool is_near_rain(const FireLevel& level, int x, int y, int z) {
  if (!level.is_raining_at)
    return false;
  return level_is_raining_at(level, x, y, z) || level_is_raining_at(level, x - 1, y, z) ||
         level_is_raining_at(level, x + 1, y, z) || level_is_raining_at(level, x, y, z - 1) ||
         level_is_raining_at(level, x, y, z + 1);
}

// The rain douse's own draw: `random.nextFloat() < 0.2f + age * 0.03f`. A null
// float source never douses, which is the weather-free behaviour a test that
// scripts no draw gets.
bool rain_douse_roll(const FireRandom& rng, int age) {
  if (!rng.next_float)
    return false;
  return rng.next_float() < 0.2f + static_cast<float>(age) * 0.03f;
}

// `FireBlock.getStateWithAge`: the state fire would take here
// (`BaseFireBlock.getState`), with `age` set when that state is plain fire.
// Soul fire has no `age` property and comes back untouched.
BlockStateKey fire_state_with_age(const FireWorld& world, int x, int y, int z, int age) {
  BlockStateKey state = fire_placement_state(world, x, y, z);
  if (state.name == kFireBlock)
    state = BlockStateKey{state.name, with_property(state.properties, "age", std::to_string(age))};
  return state;
}

// `Level.setBlock` plus `Block.onPlace`'s one effect fire cares about: a newly
// *placed* fire block schedules its first tick. `FireBlock.onPlace`'s own guard
// is `oldState.is(state.getBlock())`, so writing fire over the same block — the
// age-up write — does not reschedule (the tick already scheduled itself at its
// top), while fire written over air or over a block it just consumed does.
//
// Only `minecraft:fire` schedules: `SoulFireBlock` inherits `BaseFireBlock.onPlace`,
// which has no `scheduleTick` call at all, so soul fire is inert.
void set_and_schedule(FireLevel& level, const FireRandom& rng, int x, int y, int z,
                      const BlockStateKey& state, bool notify) {
  const BlockStateKey old = level.get_block(x, y, z);
  level.set_block(x, y, z, state, notify);
  if (!level.schedule_tick || state.name != kFireBlock)
    return;
  if (old.name == kFireBlock)
    return;
  level.schedule_tick(x, y, z, kFireTickMinDelay + rng.next_int(kFireTickDelaySpread));
}

// `Level.removeBlock(pos, false)`: air, told to the client, no drops.
void remove_block(FireLevel& level, const FireRandom& rng, int x, int y, int z) {
  set_and_schedule(level, rng, x, y, z, BlockStateKey{"minecraft:air", ""}, /*notify=*/true);
}

// `FireBlock.checkBurnOut`: does the fire at (x, y, z) consume the block beside
// it? `odds` is the block's burn odds against `chance` (300 for a horizontal
// neighbour, 250 for the one above or below), and a survivor of that roll is
// either replaced by a fresh fire or removed outright.
//
// Rain decides which of the two: `nextInt(age + 10) < 5 && !isRainingAt(pos)`
// sends a cell it is raining on down the removal branch. Rain does not stop a
// fire consuming what it is sitting on; it stops the fire *staying*.
//
// Whichever branch ran, a consumed *TNT* block is primed: vanilla's last two
// lines are `Block block = oldState.getBlock(); if (block instanceof TntBlock)
// TntBlock.prime(level, pos);`, and `oldState` is read before either write — so
// a TNT the fire burned away (or set fire to) becomes a lit entity rather than a
// pile of ash. `TNT` is on the flammability table at ignite 15 / burn 100, which
// is what makes the fire path the one that matters.
void check_burn_out(FireLevel& level, const FireRandom& rng, int x, int y, int z, int chance,
                    int age) {
  const BlockStateKey state = level.get_block(x, y, z);
  if (rng.next_int(chance) >= state_burn_odds(state))
    return;
  if (rng.next_int(age + 10) < 5 && !level_is_raining_at(level, x, y, z)) {
    const int new_age = std::min(age + rng.next_int(5) / 4, kFireMaxAge);
    set_and_schedule(level, rng, x, y, z, fire_state_with_age(level, x, y, z, new_age),
                     /*notify=*/true);
  } else {
    remove_block(level, rng, x, y, z);
  }
  if (state.name == "minecraft:tnt" && level.prime_tnt)
    level.prime_tnt(x, y, z);
}

} // namespace

// ---- names ------------------------------------------------------------------

bool fire_is_soul_base(const BlockStateKey& state) {
  // `#minecraft:soul_fire_base_blocks`.
  return state.name == "minecraft:soul_sand" || state.name == "minecraft:soul_soil";
}

bool fire_is_fire(const BlockStateKey& state) {
  // `#minecraft:fire`.
  return state.name == kFireBlock || state.name == kSoulFireBlock;
}

bool fire_is_air(const BlockStateKey& state) {
  // `Level.isEmptyBlock`.
  return state.name == "minecraft:air" || state.name == "minecraft:cave_air" ||
         state.name == "minecraft:void_air";
}

int fire_ignite_odds(std::string_view block_name) {
  const FireFlammability* row = find_flammability(block_name);
  return row ? row->ignite_odds : 0;
}

bool fire_block_can_burn(const BlockStateKey& state) {
  // `FireBlock.canBurn` is `getIgniteOdds(state) > 0`.
  return state_ignite_odds(state) > 0;
}

int fire_burn_odds(const BlockStateKey& state) {
  // `FireBlock.getBurnOdds(state)`, the table's burn column behind the same
  // waterlogged refusal.
  return state_burn_odds(state);
}

bool fire_is_infiniburn(const BlockStateKey& state) {
  // `#minecraft:infiniburn_overworld`'s two members.
  return state.name == "minecraft:netherrack" || state.name == "minecraft:magma_block";
}

int fire_age_of(const BlockStateKey& state) {
  const std::string value = block_property(state.properties, "age");
  if (value.empty())
    return 0;
  return std::atoi(value.c_str());
}

// ---- placement and survival --------------------------------------------------

BlockStateKey fire_placement_state(const FireWorld& world, int x, int y, int z) {
  // `BaseFireBlock.getState`: a soul-fire base block below makes the fire soul.
  if (world.get_block && fire_is_soul_base(world.get_block(x, y - 1, z)))
    return BlockStateKey{std::string(kSoulFireBlock), ""};
  // `FireBlock.getStateForPlacement`: with something burnable or sturdy below,
  // the default state (age 0, no face connected) is what is placed.
  if (can_burn_cell(world, x, y - 1, z) || is_sturdy(world, x, y - 1, z))
    return BlockStateKey{std::string(kFireBlock),
                         "age=0,east=false,north=false,south=false,up=false,west=false"};
  // Otherwise each connection property answers whether that neighbour burns.
  std::string props = "age=0";
  props = with_property(props, "up", can_burn_cell(world, x, y + 1, z) ? "true" : "false");
  props = with_property(props, "north", can_burn_cell(world, x, y, z - 1) ? "true" : "false");
  props = with_property(props, "south", can_burn_cell(world, x, y, z + 1) ? "true" : "false");
  props = with_property(props, "west", can_burn_cell(world, x - 1, y, z) ? "true" : "false");
  props = with_property(props, "east", can_burn_cell(world, x + 1, y, z) ? "true" : "false");
  return BlockStateKey{std::string(kFireBlock), std::move(props)};
}

bool fire_can_survive(const FireWorld& world, int x, int y, int z) {
  // The state fire would take here decides which `canSurvive` applies:
  // `SoulFireBlock` needs its base block below, `FireBlock` a sturdy one or a
  // burning neighbour.
  if (!world.get_block)
    return false;
  if (fire_is_soul_base(world.get_block(x, y - 1, z)))
    return true;
  return is_sturdy(world, x, y - 1, z) || is_valid_fire_location(world, x, y, z);
}

bool fire_can_be_placed_at(const FireWorld& world, int x, int y, int z) {
  // `BaseFireBlock.canBePlacedAt`.
  if (!world.get_block || !fire_is_air(world.get_block(x, y, z)))
    return false;
  return fire_can_survive(world, x, y, z);
}

// ---- the block's own tick ----------------------------------------------------

void fire_tick(FireLevel& level, const FireRandom& rng, int difficulty, int x, int y, int z) {
  if (!level.get_block || !level.set_block)
    return;
  const BlockStateKey original = level.get_block(x, y, z);
  // Only `minecraft:fire` has a tick of its own; soul fire is inert (it has no
  // `tick` override and never schedules itself). Dropping anything else here is
  // also what keeps a stale queue entry from looping.
  if (original.name != kFireBlock)
    return;
  // `level.scheduleTick(pos, this, getFireTickDelay(random))`, first and
  // unconditionally: a fire that survives keeps its own clock running.
  if (level.schedule_tick)
    level.schedule_tick(x, y, z, kFireTickMinDelay + rng.next_int(kFireTickDelaySpread));
  // `canSpreadFireAround` is the `FIRE_SPREAD_RADIUS_AROUND_PLAYER` gamerule,
  // whose -1 "no limit" default is the only value this server has.
  BlockStateKey state = original;
  if (!fire_can_survive(level, x, y, z))
    remove_block(level, rng, x, y, z);
  const bool infiniburn = fire_is_infiniburn(level.get_block(x, y - 1, z));
  // `state.getValue(AGE)` — read from the state as it was *before* the removal
  // above, exactly as vanilla's local `state` still holds it.
  const int age = fire_age_of(state);
  // The rain removal, before the ageing: a fire that is raining on (its own cell
  // or one of its four horizontal neighbours, so a 1-wide gap does not save it)
  // is put out on a `0.2 + age * 0.03` float draw — and returns here, so a fire
  // that goes out this tick never ages.
  if (!infiniburn && level_is_raining(level) && is_near_rain(level, x, y, z) &&
      rain_douse_roll(rng, age)) {
    remove_block(level, rng, x, y, z);
    return;
  }
  const int new_age = std::min(kFireMaxAge, age + rng.next_int(3) / 2);
  if (age != new_age) {
    // `level.setBlock(pos, state.setValue(AGE, newAge), 260)`: the age write is
    // the one update vanilla marks `UPDATE_INVISIBLE`, so the client is not told
    // and `onPlace` reschedules nothing (the block type is unchanged).
    state =
        BlockStateKey{state.name, with_property(state.properties, "age", std::to_string(new_age))};
    level.set_block(x, y, z, state, /*notify=*/false);
  }
  if (!infiniburn) {
    if (!is_valid_fire_location(level, x, y, z)) {
      // Nothing left to feed on: a fire that has run past its early ages dies
      // even on a sturdy floor.
      if (!is_sturdy(level, x, y - 1, z) || age > 3)
        remove_block(level, rng, x, y, z);
      return;
    }
    if (age == kFireMaxAge && rng.next_int(4) == 0 &&
        !fire_block_can_burn(level.get_block(x, y - 1, z))) {
      remove_block(level, rng, x, y, z);
      return;
    }
  }
  // `increased_fire_burnout` is its false default here, so vanilla's `extra` is
  // 0 and the six chances are the plain 300 / 250.
  check_burn_out(level, rng, x + 1, y, z, 300, age);
  check_burn_out(level, rng, x - 1, y, z, 300, age);
  check_burn_out(level, rng, x, y - 1, z, 250, age);
  check_burn_out(level, rng, x, y + 1, z, 250, age);
  check_burn_out(level, rng, x, y, z - 1, 300, age);
  check_burn_out(level, rng, x, y, z + 1, 300, age);
  // The spread: every cell in the -1..1 x -1..1 z -1..4 y box whose *own*
  // neighbours are flammable, at `(igniteOdds + 40 + difficulty * 7) / (age +
  // 30)` odds against a rate the height scales (100, plus 100 per block above
  // the second one up).
  for (int xx = -1; xx <= 1; ++xx) {
    for (int zz = -1; zz <= 1; ++zz) {
      for (int yy = -1; yy <= 4; ++yy) {
        if (xx == 0 && yy == 0 && zz == 0)
          continue;
        int rate = 100;
        if (yy > 1)
          rate += (yy - 1) * 100;
        const int tx = x + xx, ty = y + yy, tz = z + zz;
        const int ignite_odds = cell_ignite_odds(level, tx, ty, tz);
        if (ignite_odds <= 0)
          continue;
        const int odds = (ignite_odds + 40 + difficulty * 7) / (age + 30);
        if (odds <= 0 || rng.next_int(rate) > odds)
          continue;
        // Rain keeps fire from *starting* in a cell it is raining on. The roll
        // above is drawn either way, so the weather changes no draw count.
        if (level_is_raining(level) && is_near_rain(level, tx, ty, tz))
          continue;
        const int spread_age = std::min(kFireMaxAge, age + rng.next_int(5) / 4);
        set_and_schedule(level, rng, tx, ty, tz, fire_state_with_age(level, tx, ty, tz, spread_age),
                         /*notify=*/true);
      }
    }
  }
}

// ---- the douse ---------------------------------------------------------------

void block_face_step(int face, int* dx, int* dy, int* dz) {
  // Direction ordinals as the wire numbers them.
  static const int kStep[6][3] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                  {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};
  const int index = (face >= 0 && face < 6) ? face : 0;
  *dx = kStep[index][0];
  *dy = kStep[index][1];
  *dz = kStep[index][2];
}

std::vector<FireBlockPos> fire_douse_positions(int hit_x, int hit_y, int hit_z, int face) {
  int dx = 0, dy = 0, dz = 0;
  block_face_step(face, &dx, &dy, &dz);
  // `blockEffectPos = blockHitPos.relative(hitDirection)`: the cell the bottle
  // was in when it hit the face.
  const FireBlockPos effect{hit_x + dx, hit_y + dy, hit_z + dz};
  std::vector<FireBlockPos> out;
  out.push_back(effect);
  out.push_back(FireBlockPos{hit_x, hit_y, hit_z}); // effect.relative(opposite)
  // ...then `Direction.Plane.HORIZONTAL` around that cell.
  static const int kHorizontal[4][3] = {{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}};
  for (const auto& step : kHorizontal)
    out.push_back(FireBlockPos{effect.x + step[0], effect.y + step[1], effect.z + step[2]});
  return out;
}

DouseKind fire_douse_kind(const BlockStateKey& state) {
  // `dowseFire`'s own order of tests.
  if (fire_is_fire(state))
    return DouseKind::kFire;
  if (is_campfire(state.name) && is_lit(state))
    return DouseKind::kCampfire;
  if ((is_candle(state.name) || is_candle_cake(state.name)) && is_lit(state))
    return DouseKind::kCandle;
  return DouseKind::kNone;
}

BlockStateKey fire_doused_state(const BlockStateKey& state) {
  return BlockStateKey{state.name, with_property(state.properties, "lit", "false")};
}

bool fire_can_light(const BlockStateKey& state) {
  // `CampfireBlock.canLight` needs waterlogged=false as well as lit=false;
  // `CandleBlock.canLight` the same; a candle cake has no waterlogged.
  if (is_lit(state))
    return false;
  if (is_campfire(state.name) || is_candle(state.name))
    return prop(state, "waterlogged") != "true";
  return is_candle_cake(state.name);
}

BlockStateKey fire_lit_state(const BlockStateKey& state) {
  return BlockStateKey{state.name, with_property(state.properties, "lit", "true")};
}

// ---- the burning entity ------------------------------------------------------

bool fire_ignite(Burning& burning, bool fire_immune, bool is_player, int added_ticks) {
  if (fire_immune)
    return false;
  const int before = burning.remaining_ticks;
  // `BaseFireBlock.fireIgnite`: a negative countdown (the state a player is in
  // for a second before catching) is nudged up by one; a player already at or
  // above zero has `nextInt(1, 3)` added instead.
  if (burning.remaining_ticks < 0)
    ++burning.remaining_ticks;
  else if (is_player)
    burning.remaining_ticks += added_ticks;
  // ...and anything at or above zero then ignites for the full eight seconds.
  if (burning.remaining_ticks >= 0)
    burning.ignite_seconds(kFireSecondsOnFire);
  return burning.remaining_ticks > before;
}

bool fire_entity_inside(const FireWorld& world, double min_x, double min_y, double min_z,
                        double max_x, double max_y, double max_z, float* out_damage) {
  if (!world.get_block)
    return false;
  // `Entity.checkInsideBlocks` walks the cells the box covers, its bounds
  // pulled in by 1e-7 so a box that rests exactly on a boundary does not reach
  // into the cell above.
  const int min_cx = static_cast<int>(std::floor(min_x + kCellEpsilon));
  const int min_cy = static_cast<int>(std::floor(min_y + kCellEpsilon));
  const int min_cz = static_cast<int>(std::floor(min_z + kCellEpsilon));
  const int max_cx = static_cast<int>(std::floor(max_x - kCellEpsilon));
  const int max_cy = static_cast<int>(std::floor(max_y - kCellEpsilon));
  const int max_cz = static_cast<int>(std::floor(max_z - kCellEpsilon));
  bool inside = false;
  for (int x = min_cx; x <= max_cx; ++x) {
    for (int y = min_cy; y <= max_cy; ++y) {
      for (int z = min_cz; z <= max_cz; ++z) {
        const BlockStateKey state = world.get_block(x, y, z);
        if (!fire_is_fire(state))
          continue;
        // `BaseFireBlock.SHAPE` is the full block column, so an overlapping
        // cell is an intersecting shape.
        inside = true;
        if (out_damage)
          *out_damage = state.name == kSoulFireBlock ? kSoulFireBlockDamage : kFireBlockDamage;
      }
    }
  }
  return inside;
}

} // namespace mc::world
