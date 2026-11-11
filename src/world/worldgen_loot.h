// SPDX-License-Identifier: GPL-3.0-or-later
//
// Structure loot (Phase 4 milestone K follow-up). The pack's own chest and
// dispenser tables are generated into `worldgen_loot_table.h`; this is the
// deterministic resolver a structure piece calls while it is writing its
// block-entity records.
//
// The model is vanilla's `LootTable.getRandomItems`/`LootTable.fill` with the
// fields a generated chest carries: weighted pools, uniform/constant rolls,
// `random_chance` conditions, nested tables, and the component functions the
// item registry models. A chest is filled when the generated chunk is built,
// with a seed derived from the world seed and the chest's position, so the same
// chunk always produces the same contents and no live container is needed.
//
// What is deliberately not here:
//   * `location_check` conditions. The one in the structure tables is a biome
//     predicate on a sulfur-cave pool; the resolver has no level context, so
//     the generated table treats it as satisfied rather than deleting the pool.
//   * `set_name` and `set_ominous_bottle_amplifier`: this engine's ItemStack
//     has no custom-name or ominous-bottle component, so the item is produced
//     without them.
//   * `enchant_with_levels`' exact enchanting-table selection. The resolver
//     picks one eligible `on_random_loot` enchantment and a level derived from
//     the table's cost; `enchant_randomly` is vanilla's uniform compatible pick.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "world/item.h"

namespace mc::world {

// One rolled stack, in the item registry's terms. Components the engine models
// are carried directly; the slot is chosen by the caller (a 27-slot chest).
struct WgLootStack {
  std::string item;
  int32_t count = 1;
  int32_t damage = 0;
  int32_t potion = -1;
  std::vector<ItemEnchantment> enchantments;
  std::vector<MobEffectInstance> custom_effects;

  bool empty() const {
    return item.empty() || count <= 0;
  }
};

// The table whose `name` matches, or nullptr. The generated table is sorted by
// name.
const struct WgLootTableRow* wg_loot_table_by_name(std::string_view name);

// `LootTable.getRandomItems` for one table. `seed` is the chest's loot-table
// seed, not the world seed. Nested table entries and composite entries recurse;
// empty entries are omitted (they only mean "leave this slot empty").
std::vector<WgLootStack> wg_roll_loot_table(std::string_view table, int64_t seed);

// `RandomizableContainer.setBlockEntityLootTable`'s seed source, made
// positional: a stable function of the world seed, the chest position and the
// table name. Vanilla consumes a piece's post-process random draw instead; this
// port has its own decoration stream, so the position is the stable half both
// sides share.
int64_t wg_structure_loot_seed(int64_t world_seed, int32_t x, int32_t y, int32_t z,
                               std::string_view table);

// Converts a rolled table to the concrete stacks a chest block entity stores,
// trimming empty entries and capping a rolled count at the item's stack size.
std::vector<ItemStack> wg_structure_loot_items(std::string_view table, int64_t seed);

} // namespace mc::world
