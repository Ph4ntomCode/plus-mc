// SPDX-License-Identifier: GPL-3.0-or-later
//
// Block drops (Phase 2 milestone F). Resolves what a block break yields from
// the generated table in world/block_drops.h, which
// tools/gen_block_drops.py flattens out of the vanilla 26.2 loot tables under
// a documented model (bare hands, no enchantments, no shears).
//
// Two decisions happen here:
//   1. Tool gate — the vanilla needs_{stone,iron,diamond}_tool block tags say
//      which blocks only drop to the correct pickaxe. Bare hands (or a
//      pickaxe of too low a tier, or a non-pickaxe) yield nothing.
//   2. Loot rules — each rule belongs to a loot pool; the first rule in a
//      pool whose property requirements match the broken block's actual state
//      wins, and chance-based rules are only honoured for the block's first
//      pool (so leaves drop a sapling, not a sapling *and* a stick *and* an
//      apple).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "world/chunk.h"

namespace mc::world {

struct BlockDrop {
  std::string item; // "minecraft:cobblestone"
  int32_t count = 1;
};

// Items a break of `state` with `held_item` yields, in loot-pool order.
// `held_item` is a namespaced item name ("" = bare hands). Empty when the
// break yields nothing (no loot table, tool gate, or no matching rule).
std::vector<BlockDrop> resolve_block_drops(const BlockStateKey& state,
                                           const std::string& held_item);

// The same table with *every* pool's chance honoured: one roll per pool, each
// pool's own rule dropping when `roll()` (one uniform draw in [0, 1)) passes its
// generated `chance`. This is `Level.dropResources` as vanilla really resolves
// it, and it is what a *decayed* leaf leaves behind — a 5% sapling, a 2% stick
// and a 0.5% apple, not the guaranteed sapling the deterministic resolver above
// reports for the break path (world/leaves.h's `drop_resources` calls this).
//
// The pool model is the same as the deterministic resolver's, including its one
// limitation: the generated order puts a pool's certain rule before its chance
// ones, so a pool that holds both drops its certain rule whatever the roll says
// (gravel's 10% flint stays out). Every pool whose *only* satisfiable rule is a
// chance one — which is what the leaves' three pools are — honours it exactly.
std::vector<BlockDrop> resolve_block_drops_rolled(const BlockStateKey& state,
                                                  const std::string& held_item,
                                                  const std::function<double()>& roll);

// Pickaxe tier an item provides: 0 = not a pickaxe, 1 = wooden/golden,
// 2 = stone, 3 = iron, 4 = diamond, 5 = netherite. Mirrors the vanilla tool
// tiers; only the mining tier matters here (there is no durability model yet).
int held_pickaxe_tier(const std::string& held_item);

// ---- mob drops (Phase 3 milestone I) ---------------------------------------
//
// What a killed mob leaves behind, from the generated table in
// world/mob_loot_table.h (tools/gen_mob_loot.py flattens the vanilla entity
// loot tables under a documented model: a healthy adult mob killed by a player,
// bare-handed, no enchantments, not on fire).
//
// `killed_by_player` gates the pools that require it, so a mob that fell to its
// death drops nothing. `roll` is one uniform draw in [0, 1) and decides the
// chance pools and the count ranges, so the caller owns the randomness (and a
// test can pin it). Items whose rolled count is non-positive are omitted — that
// is how vanilla encodes "0 or 1 spider eye" as a uniform -1..1 count.
std::vector<BlockDrop> resolve_mob_drops(const std::string& mob_name, bool killed_by_player,
                                         const std::function<double()>& roll);

// Minimum pickaxe tier a block needs to drop at all. 0 = drops bare-handed;
// 1..5 = needs a pickaxe of that tier or better; 6 = the block requires a tool
// this model does not provide (shovel/shears/sword, e.g. snow, cobweb), so
// nothing ever satisfies it. Derived from vanilla's code-side
// requiresCorrectToolForDrops flag plus the needs_*_tool tier tags
// (tools/gen_block_drops.py); see world/block_drops.h.
int block_min_pickaxe_tier(const std::string& block_name);

} // namespace mc::world
