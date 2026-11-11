// SPDX-License-Identifier: GPL-3.0-or-later
//
// Keep the `assert`s live in every configuration: a Release build defines
// NDEBUG, and without this the whole file compiles to a no-op in the gate that
// runs it (the checks were only ever firing in Debug/ASan — which no longer
// even compiled, because the container helper the asserts name was included
// through this guard's absence rather than directly).
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <map>
#include <tuple>

#include "world/container.h" // block_property
#include "world/piston.h"

using namespace mc::world;
using P = std::tuple<int, int, int>;
using Blocks = std::map<P, BlockStateKey>;

namespace {

BlockStateKey at(const Blocks& blocks, int x, int y, int z) {
  auto it = blocks.find({x, y, z});
  return it == blocks.end() ? BlockStateKey{"minecraft:air", ""} : it->second;
}

PistonGet getter(const Blocks& blocks) {
  return [&blocks](int x, int y, int z) { return at(blocks, x, y, z); };
}

PistonWrite writer(Blocks& blocks) {
  return [&blocks](int x, int y, int z, const BlockStateKey& s) { blocks[{x, y, z}] = s; };
}

void test_plain_push() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  blocks[{1, 0, 0}] = {"minecraft:stone", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && move.extending && move.blocks.size() == 1);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 2, 0, 0).name == "minecraft:stone");
  assert(block_property(at(blocks, 0, 0, 0).properties, "extended") == "true");
}

void test_sticky_retraction() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:sticky_piston", "extended=true,facing=east"};
  blocks[{1, 0, 0}] = {"minecraft:air", ""};
  blocks[{2, 0, 0}] = {"minecraft:stone", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && !move.extending);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 1, 0, 0).name == "minecraft:stone");
  assert(block_property(at(blocks, 0, 0, 0).properties, "extended") == "false");
}

void test_immovable_rejects_push() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{1, 0, 0}] = {"minecraft:obsidian", ""};
  blocks[{2, 0, 0}] = {"minecraft:air", ""};
  auto get = getter(blocks);
  assert(!plan_piston_move(get, 0, 0, 0).valid);
}

// A slime block drags the movable blocks on its four perpendicular faces, and
// the chain still moves one cell at a time: the far end first, then the slime.
void test_slime_push_chain() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  blocks[{1, 0, 0}] = {"minecraft:slime_block", ""};
  blocks[{2, 0, 0}] = {"minecraft:stone", ""};
  blocks[{1, 1, 0}] = {"minecraft:stone", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && move.extending);
  assert(move.blocks.size() == 3);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 2, 0, 0).name == "minecraft:slime_block");
  assert(at(blocks, 3, 0, 0).name == "minecraft:stone");
  assert(at(blocks, 2, 1, 0).name == "minecraft:stone");
  assert(at(blocks, 1, 0, 0).name == "minecraft:air");
  assert(at(blocks, 1, 1, 0).name == "minecraft:air");
}

// `canStickToEachOther`: slime and honey never stick to each other, even
// though each sticks to an ordinary block.
void test_slime_and_honey_do_not_stick() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  blocks[{1, 0, 0}] = {"minecraft:honey_block", ""};
  blocks[{1, 1, 0}] = {"minecraft:slime_block", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && move.extending);
  assert(move.blocks.size() == 1);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 2, 0, 0).name == "minecraft:honey_block");
  assert(at(blocks, 1, 1, 0).name == "minecraft:slime_block");
}

// Twelve movable cells fit; a thirteenth does not, whatever the shape.
void test_twelve_block_limit() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  for (int x = 1; x <= 12; ++x)
    blocks[{x, 0, 0}] = {"minecraft:stone", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && move.blocks.size() == 12);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 13, 0, 0).name == "minecraft:stone");

  blocks.clear();
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  for (int x = 1; x <= 13; ++x)
    blocks[{x, 0, 0}] = {"minecraft:stone", ""};
  assert(!plan_piston_move(get, 0, 0, 0).valid);
}

// A sticky piston's retraction pulls the sticky run in front of the head, not
// just the block next to it, and branches come along.
void test_sticky_pull_chain() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:sticky_piston", "extended=true,facing=east"};
  blocks[{2, 0, 0}] = {"minecraft:slime_block", ""};
  blocks[{3, 0, 0}] = {"minecraft:stone", ""};
  blocks[{2, 1, 0}] = {"minecraft:stone", ""};
  auto get = getter(blocks);
  auto write = writer(blocks);
  auto move = plan_piston_move(get, 0, 0, 0);
  assert(move.valid && !move.extending);
  assert(move.blocks.size() == 3);
  assert(apply_piston(move, get, write));
  assert(at(blocks, 1, 0, 0).name == "minecraft:slime_block");
  assert(at(blocks, 2, 0, 0).name == "minecraft:stone");
  assert(at(blocks, 1, 1, 0).name == "minecraft:stone");
  assert(at(blocks, 3, 0, 0).name == "minecraft:air");
  assert(at(blocks, 2, 1, 0).name == "minecraft:air");
  assert(block_property(at(blocks, 0, 0, 0).properties, "extended") == "false");
}

// `isPushable`: an extended piston is immovable, an unextended one is not.
void test_extended_piston_is_immovable() {
  Blocks blocks;
  blocks[{0, 0, 0}] = {"minecraft:piston", "extended=false,facing=east"};
  blocks[{-1, 0, 0}] = {"minecraft:redstone_block", ""};
  blocks[{1, 0, 0}] = {"minecraft:slime_block", ""};
  blocks[{2, 0, 0}] = {"minecraft:piston", "extended=true,facing=east"};
  auto get = getter(blocks);
  assert(!plan_piston_move(get, 0, 0, 0).valid);
}

} // namespace

int main() {
  test_plain_push();
  test_sticky_retraction();
  test_immovable_rejects_push();
  test_slime_push_chain();
  test_slime_and_honey_do_not_stick();
  test_twelve_block_limit();
  test_sticky_pull_chain();
  test_extended_piston_is_immovable();
  std::printf("piston: all checks passed\n");
  return 0;
}
