// SPDX-License-Identifier: GPL-3.0-or-later
//
// Content adapters for the 26.1/26.2 translated play path.
//
// The server simulates one canonical 26.3 world.  A translated client has a
// different built-in block-state registry and its configuration registry gives
// it a different biome order, so both must be projected at encode time:
//   * block_states() maps canonical 26.3 state ids to the target ids generated
//     by tools/gen_block_translation.py;
//   * TargetBiomeTable maps canonical biome names to the target client's
//     advertised registry order, with a fallback for names it does not know.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "net/byte_buffer.h"
#include "world/chunk.h"
#include "world/item.h"
#include "world/registry.h"

namespace mc::translation::t0 {

// Block-state table for a translated client.  Protocol 775 (26.1, 26.1.1 and
// 26.1.2) shares the 26.1.2 registry; 776 uses 26.2.  Unknown canonical ids
// resolve to air rather than failing the whole chunk.
const world::BlockStateTable& block_states(int protocol);

// Built-in registry ids. Items and entity types are not in the synchronized
// configuration registries, so these come from the generated maps in
// t0_registries.generated.h. Both return -1 when the target release has no
// entry with the canonical name.
int32_t target_item_id(int protocol, int32_t canonical_item_id);
// Reverse lookup for a serverbound HashedStack: a translated client names the
// target release's item id, while the canonical menu compares it with its own.
// -1 when the target id has no canonical source.
int32_t canonical_item_id(int protocol, int32_t target_item_id);
int32_t target_entity_type_id(int protocol, int32_t canonical_entity_type_id);

// Writes an ItemStack with its item id and the four modelled data components
// projected to the target release: damage, enchantments, potion contents and
// the armor trim. Component type ids and their registry ids are re-mapped;
// entries the target release does not have are dropped instead of being sent
// as the wrong content. Unmodelled raw components are omitted, because their
// component ids and payload codecs also drift between releases.
// Callers should treat a 0-count write as an item the target release does not
// have.
void write_item_stack(mc::net::Writer& w, const mc::world::ItemStack& stack, int protocol);

// Target biome registry order from the client's configuration, with a fallback
// (normally plains, or the first advertised biome when plains is absent).
class TargetBiomeTable final : public world::BiomeNameTable {
public:
  TargetBiomeTable(std::vector<std::string> names, std::string fallback);
  std::optional<int32_t> lookup(const std::string& name) const override;
  size_t registry_size() const override;

private:
  world::NameListBiomeTable names_;
  int32_t fallback_ = 0;
};

} // namespace mc::translation::t0
