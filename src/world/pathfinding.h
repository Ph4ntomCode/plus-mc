// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ground pathfinding (Phase 3 milestone I: mobs).
//
// Vanilla's mob movement is `PathNavigation.createPath` -> `PathFinder` (A*)
// over nodes produced by a `WalkNodeEvaluator`: neighbours are the 8 horizontal
// directions, each tried at the current level, one step up (the "jump onto the
// block" case) and a bounded number of steps down (walking off a ledge), with a
// diagonal only allowed when the two cells it squeezes past are open. When the
// target cannot be reached the search still returns the path to the *closest*
// node it found, which is what makes a chasing zombie keep coming at a player
// standing on a pillar instead of standing still.
//
// What is modelled:
//   * WALKABLE / blocking, from a single `is_solid` predicate. The block
//     registry carries names and states, not collision shapes (see
//     world/experience_orb.h for the same approximation), so "solid" is
//     "not air and not a fluid" and a cell is standable when the block below it
//     is solid and the mob has headroom above;
//   * a mob's *height* decides how many blocks of headroom a cell needs, so a
//     0.7-tall chicken fits through a one-block gap an iron golem would not
//     (there is no per-mob collision-shape shrinking beyond this);
//   * the search is bounded (`max_nodes`), so a mob never stalls the tick.
//
// Deliberately not modelled: per-block movement costs (water, lava, fences,
// doors, trapdoors), `NodeEvaluator`'s `PathType` distinctions (BLOCKED /
// OPEN / WALKABLE / WATER / ...), swim/amphibious navigation, and being pushed.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace mc::world {

// Grid coordinates of a *standable cell*: the empty block a mob's feet occupy.
// `y` is therefore the feet level — the block at y-1 is the floor the mob stands
// on, not the floor block itself (vanilla's nodes store the floor block; this
// module stores the cell above it so a node's y is also a position's y).
struct NavCell {
  int x = 0;
  int y = 0;
  int z = 0;
};

inline bool operator==(const NavCell& a, const NavCell& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

// The cell a world position's feet are in (BlockPos.containing for the feet).
inline NavCell nav_cell_at(double x, double y, double z) {
  return NavCell{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y + 1.0e-7)),
                 static_cast<int>(std::floor(z))};
}

// The centre of a cell, as a world position the mob walks toward.
inline double nav_cell_centre_x(const NavCell& c) {
  return static_cast<double>(c.x) + 0.5;
}
inline double nav_cell_centre_z(const NavCell& c) {
  return static_cast<double>(c.z) + 0.5;
}

// GroundPathNavigation's node evaluator, reduced to one predicate plus the
// mob's height.
struct NavEnvironment {
  // Whether the block at those world coordinates stops the mob.
  std::function<bool(int, int, int)> is_solid;
  // Mob height (EntityType dimensions). A cell needs ceil(height) clear blocks
  // above its floor.
  double height = 1.95;
};

// Steps the search will try downwards from a cell; WalkNodeEvaluator's
// `maxFallDistance` is 3 for most ground mobs.
constexpr int kNavMaxFall = 3;
// Visited-node budget and the longest path handed back. Vanilla bounds the
// search by `followRange`; a fixed budget keeps this deterministic.
constexpr int kNavMaxNodes = 2048;
constexpr int kNavMaxPathLength = 96;

// A path as the sequence of cells *after* the start, i.e. waypoints to walk to
// in order. `next` is how far along the follower is.
struct NavPath {
  std::vector<NavCell> points;
  size_t next = 0;

  bool done() const {
    return next >= points.size();
  }
  size_t remaining() const {
    return next >= points.size() ? 0 : points.size() - next;
  }
  const NavCell* current() const {
    return done() ? nullptr : &points[next];
  }
  void advance() {
    if (!done())
      ++next;
  }
};

// Whether a mob of `env.height` can stand with its feet in `cell`: the block
// below is solid and every block the body occupies is clear.
bool nav_cell_walkable(const NavCell& cell, const NavEnvironment& env);

// The nearest walkable cell to `cell`, searched outwards in a cube of `radius`
// blocks (the goal a path is actually built to: a player standing on a fence or
// a mob inside a wall has no walkable cell exactly at their feet). Closer in the
// *horizontal* plane wins first, then the smaller vertical step — so a target
// buried in a block resolves to the space directly above it rather than to
// whichever side happens to be enumerated first. Returns `cell` unchanged when
// it is already walkable, and `blocked` output true when nothing standable was
// found inside the radius.
NavCell nav_nearest_walkable(const NavCell& cell, const NavEnvironment& env, int radius,
                             bool* blocked);

// A* from `from` to `to`. `from` should already be walkable (the cell the mob
// is standing in); `to` is snapped to the nearest walkable cell first. The
// returned path is empty when the mob is already there, and *partial* — the
// best cell reached — when the target is unreachable, exactly like
// PathNavigation's fallback. `reached` reports whether the goal itself was
// reached.
NavPath find_path(const NavCell& from, const NavCell& to, const NavEnvironment& env,
                  bool* reached = nullptr, int max_nodes = kNavMaxNodes);

} // namespace mc::world
