// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/block_tags.h"

#include <algorithm>

#include "world/block_tag_table.h"

namespace mc::world {

bool block_has_tag(uint8_t tag, std::string_view block_name) {
  const GeneratedBlockTag* begin = kBlockTags;
  const GeneratedBlockTag* end = kBlockTags + kBlockTagCount;
  const GeneratedBlockTag* it = std::lower_bound(
      begin, end, block_name, [](const GeneratedBlockTag& entry, std::string_view name) {
        return std::string_view(entry.name) < name;
      });
  if (it == end || std::string_view(it->name) != block_name)
    return false;
  return (it->tags & tag) != 0;
}

} // namespace mc::world
