// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "world/chunk.h"

namespace mc::world {

struct PistonBlockPos {
  int x = 0, y = 0, z = 0;
  bool operator==(const PistonBlockPos&) const = default;
};

struct PistonMove {
  PistonBlockPos piston;
  PistonBlockPos direction;
  // `PistonStructureResolver`'s `toPush`, in its own order: reverse iteration
  // and one step along the movement direction never overwrites a source. For
  // an extension the movement is `direction` (far-to-near); for a sticky pull
  // it is `-direction` (nearest-first), and the run starts two cells out.
  std::vector<PistonBlockPos> blocks;
  bool sticky = false;
  bool extending = false;
  bool valid = false;
};

using PistonGet = std::function<BlockStateKey(int, int, int)>;
using PistonWrite = std::function<void(int, int, int, const BlockStateKey&)>;

bool piston_block(std::string_view name);
PistonBlockPos piston_direction(std::string_view facing);
bool piston_powered(const PistonGet& get, int x, int y, int z);
PistonMove plan_piston_move(const PistonGet& get, int x, int y, int z);

// Applies one state transition. The callback owns notifications and persistence.
// Returns false when the piston is not powered, already in the requested state,
// or the push would violate the twelve-block vanilla limit.
bool apply_piston(PistonMove move, const PistonGet& get, const PistonWrite& write);

} // namespace mc::world
