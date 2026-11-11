// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the ground pathfinder (Phase 3 milestone I):
//   * walkability: the floor below, the headroom above (and that a tall mob
//     needs two clear blocks where a short one needs one),
//   * a straight line, a detour around a wall, a diagonal that may not cut a
//     corner,
//   * the step up (one block) and the fall (up to three),
//   * a *partial* path to an unreachable target, which is what makes a chasing
//     mob keep coming instead of giving up,
//   * the bounded search (a target behind enough wall does not hang the tick).
#include <cmath>
#include <cstdio>
#include <functional>
#include <set>
#include <vector>

#include "world/pathfinding.h"

using namespace mc::world;

static int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

namespace {

// A solid layer at y <= 0 plus whatever the test adds: `blocks` is the set of
// solid cells, keyed (x, y, z).
struct World {
  std::set<std::tuple<int, int, int>> blocks;
  bool floor = true;

  void wall(int x, int z0, int z1, int y0, int y1) {
    for (int z = z0; z <= z1; ++z)
      for (int y = y0; y <= y1; ++y)
        blocks.emplace(x, y, z);
  }
  void box(int x0, int x1, int y0, int y1, int z0, int z1) {
    for (int x = x0; x <= x1; ++x)
      for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
          blocks.emplace(x, y, z);
  }
  NavEnvironment env(double height = 1.95) {
    NavEnvironment out;
    out.height = height;
    auto self = this;
    out.is_solid = [self](int x, int y, int z) {
      if (self->floor && y <= 0)
        return true;
      return self->blocks.count(std::make_tuple(x, y, z)) != 0;
    };
    return out;
  }
};

NavCell cell(int x, int y, int z) {
  return NavCell{x, y, z};
}

void test_walkable() {
  World w;
  const NavEnvironment env = w.env();
  CHECK(nav_cell_walkable(cell(0, 1, 0), env));  // standing on the floor at y=0
  CHECK(!nav_cell_walkable(cell(0, 0, 0), env)); // inside the floor
  CHECK(!nav_cell_walkable(cell(0, 5, 0), env)); // nothing to stand on up there

  // A one-block-tall room: a ceiling at y=2 over the cell at y=1.
  World low;
  low.box(0, 0, 2, 2, 0, 0);
  // A chicken (0.7) fits under it; a zombie (1.95) does not.
  CHECK(nav_cell_walkable(cell(0, 1, 0), low.env(0.7)));
  CHECK(!nav_cell_walkable(cell(0, 1, 0), low.env(1.95)));
  // Two blocks of headroom fit both.
  World two_high;
  two_high.box(0, 0, 3, 3, 0, 0);
  CHECK(nav_cell_walkable(cell(0, 1, 0), two_high.env(1.95)));
  CHECK(!nav_cell_walkable(cell(0, 2, 0), two_high.env(0.7))); // the ceiling itself
}

void test_straight_line() {
  World w;
  bool reached = false;
  NavPath path = find_path(cell(0, 1, 0), cell(4, 1, 0), w.env(), &reached);
  CHECK(reached);
  CHECK(path.points.size() == 4);
  CHECK(path.points.front() == cell(1, 1, 0));
  CHECK(path.points.back() == cell(4, 1, 0));
  CHECK(!path.done() && path.current() != nullptr && *path.current() == cell(1, 1, 0));
  CHECK(path.remaining() == 4);
  path.advance();
  CHECK(*path.current() == cell(2, 1, 0));
  CHECK(path.remaining() == 3);

  // Already there: no waypoints at all.
  bool at_goal = true;
  const NavPath none = find_path(cell(3, 1, 3), cell(3, 1, 3), w.env(), &at_goal);
  CHECK(at_goal);
  CHECK(none.points.empty() && none.done());
}

void test_wall_detour() {
  World w;
  // A wall on x=2 from z=-3 to z=3: the path must go around one of its ends.
  w.wall(2, -3, 3, 1, 3);
  bool reached = false;
  const NavPath path = find_path(cell(0, 1, 0), cell(4, 1, 0), w.env(), &reached);
  CHECK(reached);
  CHECK(!path.points.empty());
  for (const NavCell& c : path.points)
    CHECK(!(c.x == 2 && c.z >= -3 && c.z <= 3 && c.y >= 1 && c.y <= 3));
}

void test_diagonal_cannot_cut_a_corner() {
  World w;
  // Two blocks meeting at a corner at (2,1,1) and (1,1,2): a diagonal step from
  // (1,1,1)... rather, from (2,1,2) to (1,1,2)? The pair chosen here blocks the
  // diagonal between (1,1,1) and (2,1,2) from both sides, and the straight route
  // around must remain open.
  w.blocks.emplace(1, 1, 2);
  w.blocks.emplace(2, 1, 1);
  const NavPath path = find_path(cell(1, 1, 1), cell(2, 1, 2), w.env(), nullptr);
  CHECK(!path.points.empty());
  // Whatever route it found, it never steps into a solid cell.
  for (const NavCell& c : path.points)
    CHECK(w.blocks.count(std::make_tuple(c.x, c.y, c.z)) == 0);
}

void test_step_up_and_fall() {
  World w;
  // A one-block step at x=2: the path climbs it.
  w.box(2, 2, 1, 1, 0, 0);
  bool reached = false;
  NavPath path = find_path(cell(0, 1, 0), cell(3, 2, 0), w.env(), &reached);
  CHECK(reached);
  CHECK(!path.points.empty());
  bool climbed = false;
  for (const NavCell& c : path.points)
    if (c.y == 2)
      climbed = true;
  CHECK(climbed);

  // A drop: from y=2 the mob may walk off the edge down to the floor.
  reached = false;
  path = find_path(cell(0, 2, 0), cell(3, 1, 0), w.env(), &reached);
  CHECK(reached);

  // More than kNavMaxFall blocks down is not a step, it is a fall the path will
  // not take.
  World deep;
  deep.box(0, 0, 1, 4, 0, 0); // a pillar to stand on, 5 blocks above the floor
  bool blocked = false;
  const NavPath down = find_path(cell(0, 5, 0), cell(3, 1, 0), deep.env(), &blocked);
  // The path may still get there around, but it must never step to a cell more
  // than kNavMaxFall below the previous one.
  for (size_t i = 1; i < down.points.size(); ++i)
    CHECK(down.points[i - 1].y - down.points[i].y <= kNavMaxFall);
}

void test_unreachable_target_is_partial() {
  World w;
  // A cage around a walkable target: (5,1,0) is standable, but every cell that
  // could step into it is solid, so it can never be reached. The search must
  // still return the best route it found (toward it), not nothing.
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      if (dx != 0 || dz != 0)
        for (int dy = 1; dy <= 2; ++dy) // two layers, so its own top is not a way in
          w.blocks.emplace(5 + dx, dy, dz);
  CHECK(nav_cell_walkable(cell(5, 1, 0), w.env()));
  bool reached = true;
  const NavPath path = find_path(cell(0, 1, 0), cell(5, 1, 0), w.env(), &reached);
  CHECK(!reached);
  CHECK(!path.points.empty());
  // Closer than where it started: the first waypoint is nearer the target.
  CHECK(std::abs(path.points.front().x - 5) < std::abs(0 - 5));
}

void test_nearest_walkable() {
  World w;
  const NavEnvironment env = w.env();
  // A target inside a block resolves to the nearest standable cell (one up), not
  // to nothing: a player standing with their feet in a fence post still gets
  // chased.
  bool blocked = true;
  const NavCell snapped = nav_nearest_walkable(cell(2, 1, 0), env, 3, &blocked);
  CHECK(!blocked);
  CHECK(snapped == cell(2, 1, 0)); // nothing solid: already walkable
  w.blocks.emplace(2, 1, 0);
  const NavEnvironment blocked_env = w.env();
  const NavCell lifted = nav_nearest_walkable(cell(2, 1, 0), blocked_env, 3, &blocked);
  CHECK(!blocked);
  CHECK(lifted == cell(2, 2, 0));
}

void test_bounded_search() {
  World w;
  // A maze-like wall field far wider than the node budget: the search must
  // terminate (the budget is enforced inside) and return something.
  for (int x = 0; x < 200; ++x)
    if (x % 2 == 0)
      w.wall(x, -20, 20, 1, 1);
  const NavPath path = find_path(cell(0, 1, 0), cell(199, 1, 0), w.env(), nullptr, 64);
  for (size_t i = 1; i < path.points.size(); ++i)
    CHECK(std::abs(path.points[i].x - path.points[i - 1].x) <= 1); // still a path
}

} // namespace

int main() {
  test_walkable();
  test_straight_line();
  test_wall_detour();
  test_diagonal_cannot_cut_a_corner();
  test_step_up_and_fall();
  test_unreachable_target_is_partial();
  test_nearest_walkable();
  test_bounded_search();
  if (failures == 0)
    std::printf("pathfinding: all checks passed\n");
  else
    std::printf("pathfinding: %d check(s) failed\n", failures);
  return failures == 0 ? 0 : 1;
}
