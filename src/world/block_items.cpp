// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/block_items.h"

#include <algorithm>

#include "world/block_item_table.h"

namespace mc::world {

std::string_view block_item_block(std::string_view item_name) {
  const GeneratedBlockItem* begin = kBlockItems;
  const GeneratedBlockItem* end = kBlockItems + kBlockItemCount;
  const GeneratedBlockItem* it = std::lower_bound(
      begin, end, item_name, [](const GeneratedBlockItem& entry, std::string_view name) {
        return std::string_view(entry.item) < name;
      });
  if (it == end || std::string_view(it->item) != item_name)
    return {};
  return it->block;
}

} // namespace mc::world
