// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone F: block-drop resolution against the generated table
// (world/block_drops.h). Pure logic, no world or sockets: it pins the three
// behaviours that a regression would silently break —
//   1. the tool gate (requiresCorrectToolForDrops + needs_*_tool tiers),
//   2. property-conditional rules (wheat age, double slabs),
//   3. the chance-rule rule (a secondary chance pool never becomes a
//      guaranteed drop).
#include <cstdio>
#include <string>
#include <vector>

#include "world/chunk.h"
#include "world/loot.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  if (ok)
    return;
  std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  ++g_failures;
}

mc::world::BlockStateKey block(const char* name, const char* props = "") {
  return mc::world::BlockStateKey{name, props};
}

// Formats a drop list as "item xN, ..." for readable failures.
std::string describe(const std::vector<mc::world::BlockDrop>& drops) {
  std::string out;
  for (const auto& d : drops) {
    if (!out.empty())
      out += ", ";
    out += d.item + " x" + std::to_string(d.count);
  }
  return out.empty() ? "(nothing)" : out;
}

void expect_drops(const char* name, const char* props, const char* held,
                  const std::vector<std::pair<const char*, int>>& want) {
  const auto got = mc::world::resolve_block_drops(block(name, props), held);
  std::vector<std::pair<std::string, int>> actual;
  for (const auto& d : got)
    actual.emplace_back(d.item, d.count);
  std::vector<std::pair<std::string, int>> expected;
  for (const auto& w : want)
    expected.emplace_back(w.first, w.second);
  if (actual != expected)
    check(false, std::string(name) + (props[0] ? std::string("[") + props + "]" : "") +
                     " held=" + (held[0] ? held : "(bare)") + " -> " + describe(got));
}

} // namespace

int main() {
  using mc::world::block_min_pickaxe_tier;
  using mc::world::held_pickaxe_tier;

  // --- pickaxe tiers -------------------------------------------------------
  check(held_pickaxe_tier("") == 0, "bare hands are not a pickaxe");
  check(held_pickaxe_tier("minecraft:wooden_pickaxe") == 1, "wooden pickaxe is tier 1");
  check(held_pickaxe_tier("minecraft:stone_pickaxe") == 2, "stone pickaxe is tier 2");
  check(held_pickaxe_tier("minecraft:iron_pickaxe") == 3, "iron pickaxe is tier 3");
  check(held_pickaxe_tier("minecraft:diamond_pickaxe") == 4, "diamond pickaxe is tier 4");
  check(held_pickaxe_tier("minecraft:netherite_pickaxe") == 5, "netherite pickaxe is tier 5");
  check(held_pickaxe_tier("minecraft:iron_sword") == 0, "a sword is not a pickaxe");

  // requiresCorrectToolForDrops: stone needs *any* pickaxe, ores a tier.
  check(block_min_pickaxe_tier("minecraft:stone") == 1, "stone needs any pickaxe");
  check(block_min_pickaxe_tier("minecraft:cobblestone") == 1, "cobblestone needs any pickaxe");
  check(block_min_pickaxe_tier("minecraft:iron_ore") == 2, "iron ore needs a stone pickaxe");
  check(block_min_pickaxe_tier("minecraft:diamond_ore") == 3, "diamond ore needs an iron pickaxe");
  check(block_min_pickaxe_tier("minecraft:obsidian") == 4, "obsidian needs a diamond pickaxe");
  check(block_min_pickaxe_tier("minecraft:snow") == 6,
        "snow needs a shovel we do not model (never drops)");

  // --- tool gate -----------------------------------------------------------
  expect_drops("minecraft:stone", "", "", {});                     // bare hand
  expect_drops("minecraft:stone", "", "minecraft:iron_sword", {}); // wrong tool
  expect_drops("minecraft:stone", "", "minecraft:wooden_pickaxe", {{"minecraft:cobblestone", 1}});
  expect_drops("minecraft:stone", "", "minecraft:iron_pickaxe", {{"minecraft:cobblestone", 1}});
  expect_drops("minecraft:cobblestone", "", "", {});
  expect_drops("minecraft:cobblestone", "", "minecraft:wooden_pickaxe",
               {{"minecraft:cobblestone", 1}});
  expect_drops("minecraft:diamond_ore", "", "minecraft:stone_pickaxe", {});
  expect_drops("minecraft:diamond_ore", "", "minecraft:iron_pickaxe", {{"minecraft:diamond", 1}});
  expect_drops("minecraft:obsidian", "", "minecraft:iron_pickaxe", {});
  expect_drops("minecraft:obsidian", "", "minecraft:diamond_pickaxe", {{"minecraft:obsidian", 1}});
  expect_drops("minecraft:snow", "", "", {});
  expect_drops("minecraft:snow", "", "minecraft:iron_pickaxe", {}); // still no shovel
  expect_drops("minecraft:cobweb", "", "", {});

  // --- blocks that need no tool -------------------------------------------
  expect_drops("minecraft:dirt", "", "", {{"minecraft:dirt", 1}});
  expect_drops("minecraft:grass_block", "", "", {{"minecraft:dirt", 1}});
  expect_drops("minecraft:oak_log", "", "", {{"minecraft:oak_log", 1}});
  expect_drops("minecraft:sand", "", "", {{"minecraft:sand", 1}});

  // --- chance rules: only the first pool's chance drop is honoured ---------
  // Leaves drop a sapling; the stick/apple chance pools are not guaranteed.
  expect_drops("minecraft:oak_leaves", "", "", {{"minecraft:oak_sapling", 1}});
  // Gravel's deterministic path wins over the 10% flint.
  expect_drops("minecraft:gravel", "", "", {{"minecraft:gravel", 1}});

  // --- property-conditional rules -----------------------------------------
  expect_drops("minecraft:wheat", "age=0", "", {{"minecraft:wheat_seeds", 1}});
  expect_drops("minecraft:wheat", "age=7", "",
               {{"minecraft:wheat", 1}, {"minecraft:wheat_seeds", 1}});
  expect_drops("minecraft:acacia_slab", "type=bottom", "", {{"minecraft:acacia_slab", 1}});
  expect_drops("minecraft:acacia_slab", "type=double", "", {{"minecraft:acacia_slab", 2}});

  // --- nothing -------------------------------------------------------------
  expect_drops("minecraft:air", "", "", {});
  expect_drops("minecraft:not_a_block", "", "", {});

  if (g_failures == 0)
    std::printf("PASS: loot\t(drops, tool gate, property rules)\n");
  return g_failures == 0 ? 0 : 1;
}
