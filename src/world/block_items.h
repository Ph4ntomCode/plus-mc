// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which block an item places, over the generated table in
// world/block_item_table.h (tools/gen_block_item_table.py).
//
// The placement path asks two questions of it: whether the held item is a block
// item at all (only one of those can be placed), and which block it is (usually
// the item's own name, but `minecraft:wheat_seeds` places `minecraft:wheat`).
// The item whose name *is* a block without being a block item — `minecraft:wheat`
// the harvested crop — is absent from the table, which is what keeps it from
// planting itself.
#pragma once

#include <string_view>

namespace mc::world {

// The block `item_name` places, or an empty view when the item is not a block
// item (`BlockItem` in vanilla) or is not an item this server knows.
std::string_view block_item_block(std::string_view item_name);

} // namespace mc::world
