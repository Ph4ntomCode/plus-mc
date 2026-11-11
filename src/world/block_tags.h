// SPDX-License-Identifier: GPL-3.0-or-later
//
// The block-tag lookups the block-tick loop needs, over the flattened table in
// world/block_tag_table.h (tools/gen_block_tags.py).
//
// Only the two tags the random tick actually asks about are carried: what a
// crop grows on (`BlockTags.GROWS_CROPS`) and what holds a leaf up
// (`BlockTags.PREVENTS_NEARBY_LEAF_DECAY`). Both are membership questions with
// no state component, so a name is the whole lookup.
#pragma once

#include <cstdint>
#include <string_view>

#include "world/block_tag_table.h" // the generated masks and membership

namespace mc::world {

// `BlockState.is(BlockTags.X)`. `tag` is one of the `kBlockTag*` masks in
// world/block_tag_table.h.
bool block_has_tag(uint8_t tag, std::string_view block_name);

} // namespace mc::world
