// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Level.clip` for a voxel world this engine only knows as solid-or-not: the
// Amanatides & Woo grid walk, keeping the parameter at which the ray crosses the
// next voxel boundary in each axis and taking the smallest each step.
//
// It lives on its own because two entity models need the identical walk — the
// thrown potions (world/potion_projectile.cpp) and the arrows and thrown items
// (world/projectile.cpp) — and the one thing worth keeping identical is the
// geometry. The *predicate* is the caller's: `ClipContext.Block.COLLIDER` in
// vanilla, which this engine stands in for with "not air and not a fluid" (see
// world/potion_projectile.h for what that leaves out).
#pragma once

#include <functional>

namespace mc::world {

// Where a segment from `from` to `from + delta` first enters a solid cell. The
// voxel the segment *starts* in is not tested — vanilla's clip reports a cell
// the segment enters, and nothing here un-sticks an entity that spawned inside a
// block (the same gap world/experience_orb.h and world/mob.h document).
struct SolidHit {
  bool hit = false;
  // The point the segment entered the cell at, which is on the face the ray
  // came through (a segment that starts inside a solid cell yields `from`
  // itself).
  double x = 0.0, y = 0.0, z = 0.0;
  // The cell that was entered.
  int block_x = 0, block_y = 0, block_z = 0;
  // The face it entered through, as the Direction ordinal the wire uses (0
  // down, 1 up, 2 north, 3 south, 4 west, 5 east): the *opposite* of the step
  // taken into the cell, because a face points back at where the ray came from.
  int face = 1;
};

// An empty `is_solid` (or a zero delta) is no hit at all.
SolidHit clip_solids(const std::function<bool(int, int, int)>& is_solid, double from_x,
                     double from_y, double from_z, double dx, double dy, double dz);

} // namespace mc::world
