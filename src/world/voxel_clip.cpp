// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared voxel clip. See world/voxel_clip.h for what it mirrors and where
// the boundary sits.
#include "world/voxel_clip.h"

#include <cmath>
#include <limits>
#include <utility>

namespace mc::world {

SolidHit clip_solids(const std::function<bool(int, int, int)>& is_solid, double from_x,
                     double from_y, double from_z, double dx, double dy, double dz) {
  SolidHit result;
  if (!is_solid || (dx == 0.0 && dy == 0.0 && dz == 0.0))
    return result;
  const double from[3] = {from_x, from_y, from_z};
  const double delta[3] = {dx, dy, dz};
  int cell[3] = {static_cast<int>(std::floor(from_x)), static_cast<int>(std::floor(from_y)),
                 static_cast<int>(std::floor(from_z))};
  int step[3] = {0, 0, 0};
  double t_delta[3] = {0.0, 0.0, 0.0};
  double t_max[3] = {0.0, 0.0, 0.0};
  const double kInfinity = std::numeric_limits<double>::infinity();
  for (int axis = 0; axis < 3; ++axis) {
    if (delta[axis] > 0.0) {
      step[axis] = 1;
      t_delta[axis] = 1.0 / delta[axis];
      t_max[axis] = (static_cast<double>(cell[axis]) + 1.0 - from[axis]) / delta[axis];
    } else if (delta[axis] < 0.0) {
      step[axis] = -1;
      t_delta[axis] = -1.0 / delta[axis];
      t_max[axis] = (static_cast<double>(cell[axis]) - from[axis]) / delta[axis];
    } else {
      t_delta[axis] = kInfinity;
      t_max[axis] = kInfinity;
    }
  }
  // The guard bounds a pathological delta (a NaN sanitized into a huge number)
  // rather than the common case: a movement is at most a few voxels.
  for (int guard = 0; guard < 1024; ++guard) {
    int axis = 0;
    if (t_max[1] < t_max[0])
      axis = 1;
    if (t_max[2] < t_max[axis])
      axis = 2;
    const double t = t_max[axis];
    if (!(t <= 1.0))
      break;
    cell[axis] += step[axis];
    t_max[axis] += t_delta[axis];
    if (!is_solid(cell[0], cell[1], cell[2]))
      continue;
    result.hit = true;
    // `t` is the parameter at which the segment *enters* `cell`, i.e. the face
    // of the solid block it was stopped by.
    result.x = from[0] + delta[0] * t;
    result.y = from[1] + delta[1] * t;
    result.z = from[2] + delta[2] * t;
    result.block_x = cell[0];
    result.block_y = cell[1];
    result.block_z = cell[2];
    // Stepping *up* into a cell means the ray came through that cell's socket
    // on the low side, which is the `Direction` pointing the other way:
    // +x -> WEST, -x -> EAST, +y -> DOWN, -y -> UP, +z -> NORTH, -z -> SOUTH.
    static const int kEnteredFace[3][2] = {{4, 5}, {0, 1}, {2, 3}};
    result.face = kEnteredFace[axis][step[axis] > 0 ? 0 : 1];
    return result;
  }
  return result;
}

} // namespace mc::world
