// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/piston.h"

#include "world/container.h"
#include <algorithm>

namespace mc::world {
namespace {

constexpr int kMaxPushDepth = 12;

bool air(const BlockStateKey& s) {
  return s.name == "minecraft:air" || s.name == "minecraft:cave_air" ||
         s.name == "minecraft:void_air";
}
bool immovable(const BlockStateKey& s) {
  if (air(s))
    return false;
  if (s.name == "minecraft:obsidian" || s.name == "minecraft:crying_obsidian" ||
      s.name == "minecraft:bedrock" || s.name == "minecraft:reinforced_deepslate" ||
      s.name == "minecraft:extended_piston" || s.name == "minecraft:moving_piston" ||
      s.name == "minecraft:piston_head")
    return true;
  // `PistonBaseBlock.isPushable`: an extended piston is immovable, an
  // unextended one is an ordinary movable block. The old name-only check missed
  // the extended state and would have dragged a piston out of its own head.
  return piston_block(s.name) && block_property(s.properties, "extended") == "true";
}
PistonBlockPos add(PistonBlockPos a, PistonBlockPos b, int n = 1) {
  return {a.x + b.x * n, a.y + b.y * n, a.z + b.z * n};
}
PistonBlockPos negate(PistonBlockPos d) {
  return {-d.x, -d.y, -d.z};
}
bool same(PistonBlockPos a, PistonBlockPos b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

// `Direction.getAxis()`, as an index rather than an enum: 0 = x, 1 = y, 2 = z.
int axis_of(PistonBlockPos d) {
  if (d.y != 0)
    return 1;
  return d.z != 0 ? 2 : 0;
}

bool is_sticky(const BlockStateKey& s) {
  return s.name == "minecraft:slime_block" || s.name == "minecraft:honey_block";
}

// `PistonStructureResolver.canStickToEachOther`: slime and honey do not stick
// to each other; either one sticks to anything else.
bool can_stick_to_each_other(const BlockStateKey& a, const BlockStateKey& b) {
  const bool a_slime = a.name == "minecraft:slime_block";
  const bool b_slime = b.name == "minecraft:slime_block";
  const bool a_honey = a.name == "minecraft:honey_block";
  const bool b_honey = b.name == "minecraft:honey_block";
  if ((a_honey && b_slime) || (a_slime && b_honey))
    return false;
  return is_sticky(a) || is_sticky(b);
}

constexpr PistonBlockPos kDirections[6] = {{0, -1, 0}, {0, 1, 0},  {0, 0, -1},
                                           {0, 0, 1},  {-1, 0, 0}, {1, 0, 0}};

// A port of `PistonStructureResolver`. The list it builds is the vanilla one:
// `toPush` is ordered so that iterating it *backwards* and moving each cell one
// step along `push` never overwrites a source, including the reordering case
// where a sticky branch meets a line that was already collected.
struct Planner {
  const PistonGet& get;
  PistonBlockPos piston;
  PistonBlockPos push;
  std::vector<PistonBlockPos> to_push;

  bool pushable(const BlockStateKey& s) const {
    return air(s) || !immovable(s);
  }

  int index_of(PistonBlockPos p) const {
    for (size_t i = 0; i < to_push.size(); ++i)
      if (same(to_push[i], p))
        return static_cast<int>(i);
    return -1;
  }

  bool add_line(PistonBlockPos start, PistonBlockPos direction) {
    const BlockStateKey next_state = get(start.x, start.y, start.z);
    if (air(next_state))
      return true;
    if (!pushable(next_state) || same(start, piston))
      return true;
    if (index_of(start) >= 0)
      return true;
    int block_count = 1;
    if (block_count + static_cast<int>(to_push.size()) > kMaxPushDepth)
      return false;

    // The run of sticky blocks *behind* the start, which vanilla walks before
    // it appends the line: a sticky block drags the sticky run it is part of.
    BlockStateKey previous = next_state;
    while (is_sticky(previous)) {
      const PistonBlockPos pos = add(start, direction, -block_count);
      const BlockStateKey state = get(pos.x, pos.y, pos.z);
      if (air(state) || !can_stick_to_each_other(previous, state) || !pushable(state) ||
          same(pos, piston))
        break;
      previous = state;
      if (++block_count + static_cast<int>(to_push.size()) > kMaxPushDepth)
        return false;
    }

    int blocks_added = 0;
    for (int i = block_count - 1; i >= 0; --i) {
      to_push.push_back(add(start, direction, -i));
      ++blocks_added;
    }

    for (int i = 1;; ++i) {
      const PistonBlockPos pos = add(start, direction, i);
      const int collision = index_of(pos);
      if (collision >= 0) {
        // Reorder around the collision exactly as `reorderListAtCollision`
        // does, then resolve the branches of everything up to the join.
        std::vector<PistonBlockPos> head(to_push.begin(), to_push.begin() + collision);
        std::vector<PistonBlockPos> last_line(to_push.end() - blocks_added, to_push.end());
        std::vector<PistonBlockPos> collision_to_line(to_push.begin() + collision,
                                                      to_push.end() - blocks_added);
        to_push = std::move(head);
        to_push.insert(to_push.end(), last_line.begin(), last_line.end());
        to_push.insert(to_push.end(), collision_to_line.begin(), collision_to_line.end());
        const int limit = std::min(collision + blocks_added, static_cast<int>(to_push.size()) - 1);
        for (int j = 0; j <= limit; ++j) {
          const PistonBlockPos p = to_push[static_cast<size_t>(j)];
          const BlockStateKey s = get(p.x, p.y, p.z);
          if (is_sticky(s) && !add_branching(p))
            return false;
        }
        return true;
      }
      const BlockStateKey state = get(pos.x, pos.y, pos.z);
      if (air(state))
        return true;
      if (!pushable(state) || same(pos, piston))
        return false;
      if (static_cast<int>(to_push.size()) >= kMaxPushDepth)
        return false;
      to_push.push_back(pos);
      ++blocks_added;
    }
  }

  // `addBranchingBlocks`: only the four faces perpendicular to the movement
  // axis can drag a neighbour, and only when the pair sticks.
  bool add_branching(PistonBlockPos from) {
    const BlockStateKey from_state = get(from.x, from.y, from.z);
    for (const PistonBlockPos& direction : kDirections) {
      if (axis_of(direction) == axis_of(push))
        continue;
      const PistonBlockPos neighbour = add(from, direction);
      const BlockStateKey neighbour_state = get(neighbour.x, neighbour.y, neighbour.z);
      if (!can_stick_to_each_other(neighbour_state, from_state))
        continue;
      if (!add_line(neighbour, direction))
        return false;
    }
    return true;
  }

  bool resolve(PistonBlockPos start) {
    if (!add_line(start, push))
      return false;
    // The list grows while it is walked: a sticky block's branches are lines of
    // their own, and each new sticky cell owes the same question.
    for (size_t i = 0; i < to_push.size(); ++i) {
      const PistonBlockPos p = to_push[i];
      const BlockStateKey s = get(p.x, p.y, p.z);
      if (is_sticky(s) && !add_branching(p))
        return false;
    }
    return true;
  }
};

} // namespace

bool piston_block(std::string_view name) {
  return name == "minecraft:piston" || name == "minecraft:sticky_piston";
}

PistonBlockPos piston_direction(std::string_view facing) {
  if (facing == "north")
    return {0, 0, -1};
  if (facing == "south")
    return {0, 0, 1};
  if (facing == "west")
    return {-1, 0, 0};
  if (facing == "east")
    return {1, 0, 0};
  if (facing == "up")
    return {0, 1, 0};
  if (facing == "down")
    return {0, -1, 0};
  return {0, 0, 0};
}

bool piston_powered(const PistonGet& get, int x, int y, int z) {
  const BlockStateKey piston = get(x, y, z);
  if (!piston_block(piston.name))
    return false;
  const PistonBlockPos d = piston_direction(block_property(piston.properties, "facing"));
  constexpr int dx[6] = {0, 0, 0, 0, -1, 1};
  constexpr int dy[6] = {-1, 1, 0, 0, 0, 0};
  constexpr int dz[6] = {0, 0, -1, 1, 0, 0};
  for (int i = 0; i < 6; ++i) {
    const BlockStateKey n = get(x + dx[i], y + dy[i], z + dz[i]);
    if (n.name == "minecraft:redstone_block" ||
        (n.name == "minecraft:redstone_wire" && block_property(n.properties, "power") != "0") ||
        ((n.name == "minecraft:lever" || n.name.find("_button") != std::string_view::npos ||
          n.name.find("_pressure_plate") != std::string_view::npos ||
          n.name == "minecraft:repeater") &&
         block_property(n.properties, "powered") == "true"))
      return true;
  }
  (void)d;
  return false;
}

PistonMove plan_piston_move(const PistonGet& get, int x, int y, int z) {
  PistonMove out;
  out.piston = {x, y, z};
  const BlockStateKey p = get(x, y, z);
  if (!piston_block(p.name))
    return out;
  out.direction = piston_direction(block_property(p.properties, "facing"));
  out.sticky = p.name == "minecraft:sticky_piston";
  out.extending = piston_powered(get, x, y, z);
  const bool extended = block_property(p.properties, "extended") == "true";
  if (out.direction == PistonBlockPos{} || extended == out.extending)
    return out;

  if (out.extending) {
    Planner planner{get, out.piston, out.direction, {}};
    if (!planner.resolve(add(out.piston, out.direction)))
      return out; // a push that cannot resolve does not extend at all
    out.blocks = std::move(planner.to_push);
    out.valid = true;
    return out;
  }

  // Retraction itself is always possible; what it pulls is optional.
  out.valid = true;
  if (out.sticky) {
    // `PistonStructureResolver`'s retracting arm starts two cells out and walks
    // *toward* the piston, which is what makes the sticky run in front of the
    // head move with it rather than being left behind.
    Planner planner{get, out.piston, negate(out.direction), {}};
    if (!planner.resolve(add(out.piston, out.direction, 2)))
      planner.to_push.clear(); // `moveBlocks` returns before moving anything
    out.blocks = std::move(planner.to_push);
  }
  return out;
}

bool apply_piston(PistonMove move, const PistonGet& get, const PistonWrite& write) {
  if (!move.valid || !get || !write)
    return false;
  const BlockStateKey piston = get(move.piston.x, move.piston.y, move.piston.z);
  if (!piston_block(piston.name))
    return false;
  const PistonBlockPos d = move.direction;
  const PistonBlockPos movement = move.extending ? d : negate(d);

  // The list is ordered so a reverse walk never overwrites a source: a push
  // moves the far end first, a pull moves the cell nearest the piston first.
  for (auto it = move.blocks.rbegin(); it != move.blocks.rend(); ++it) {
    const PistonBlockPos dst = add(*it, movement);
    write(dst.x, dst.y, dst.z, get(it->x, it->y, it->z));
    write(it->x, it->y, it->z, {"minecraft:air", ""});
  }
  write(move.piston.x, move.piston.y, move.piston.z,
        {piston.name,
         with_property(piston.properties, "extended", move.extending ? "true" : "false")});
  return true;
}

} // namespace mc::world
