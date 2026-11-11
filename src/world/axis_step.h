// SPDX-License-Identifier: GPL-3.0-or-later
//
// `Direction.axisStepOrder`: the order `Entity.move` resolves a movement's three
// axes in. One rule, three entities — primed TNT, experience orbs and mobs all
// move through `Entity.move(MoverType.SELF, delta)`, and all three used to spell
// the order out as X, Y, Z.
//
//     |dx| < |dz|  ?  (Y, Z, X)  :  (Y, X, Z)
//
// Y is always first; whichever horizontal axis the entity is travelling along
// harder goes before the other. The order is read from the movement *before*
// any of it is resolved (`Entity.collideWithShapes(movement, ...)`), so it is a
// property of the step, not of the collision it runs into.
//
// It is observable. A box that slides along a wall while it falls lands
// differently depending on whether it falls before or after it slides, and the
// horizontal pairing picks which of two diagonal contacts wins — which is why
// vanilla stopped hard-coding X-then-Z (the old `Entity.move` order this engine
// was written against) and the reason a TNT cannon's casing throws the charge
// the way it does.
#pragma once

#include <cmath>

namespace mc::world {

// The axis indices this engine uses: 0 = x, 1 = y, 2 = z (the `axis` parameter
// of the movers, and of `BlockPosKey`'s components). Writes all three, in order.
inline void axis_step_order(double dx, double dz, int* out) {
  out[0] = 1; // y
  if (std::abs(dx) < std::abs(dz)) {
    out[1] = 2; // z, then x
    out[2] = 0;
  } else {
    out[1] = 0; // x, then z
    out[2] = 2;
  }
}

} // namespace mc::world
