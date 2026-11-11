// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/registry.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "world/block_states.h"
#include "world/default_block_states.h"
#include "world/observed_block_states.h"

namespace mc::world {

std::optional<int32_t> StaticBlockStateTable::lookup(const BlockStateKey& key) const {
  for (const auto& [state, id] : states_)
    if (state == key)
      return id;
  return std::nullopt;
}

namespace {
// Hash table over the complete generated registry, split over two levels so a
// lookup never builds a key: the outer map is the block *name* and the inner
// one that name's property strings, both of which the caller already holds.
// The single-level form had to concatenate `name + '\0' + props` into a fresh
// std::string per call — a heap allocation per lookup (block names outgrow the
// small-string buffer), on paths that resolve a state per block write and per
// palette entry. Splitting also stops the name from being stored once per
// state: only the (much shorter) property strings repeat.
class HashBlockStateTable final : public BlockStateTable {
public:
  HashBlockStateTable() {
    by_name_.reserve(kBlockStateCount / 8);
    for (const GeneratedBlockState& e : kBlockStates)
      by_name_[std::string(e.name)].emplace(std::string(e.props), e.id);
  }
  std::optional<int32_t> lookup(const BlockStateKey& key) const override {
    const auto name = by_name_.find(key.name);
    if (name == by_name_.end())
      return std::nullopt;
    const auto props = name->second.find(key.properties);
    if (props == name->second.end())
      return std::nullopt;
    return props->second;
  }

private:
  std::unordered_map<std::string, std::unordered_map<std::string, int32_t>> by_name_;
};
} // namespace

namespace {
// Concrete singleton the two public accessors share (so observed_block_states
// can merge its entries without losing the concrete type).
const StaticBlockStateTable& milestone_a_entries() {
  // Network block-state ids for protocol 777 / 26.3, byte-verified against
  // the committed fixture capture and the 26.3 block registry (see registry.h).
  static const StaticBlockStateTable table({
      {{"minecraft:air", ""}, 0},
      {{"minecraft:bedrock", ""}, 88},
      {{"minecraft:dirt", ""}, 10},
      {{"minecraft:grass_block", "snowy=false"}, 9},
  });
  return table;
}
} // namespace

const BlockStateTable& milestone_a_block_states() {
  return milestone_a_entries();
}

const BlockStateTable& full_block_states() {
  static const HashBlockStateTable table;
  return table;
}

std::optional<BlockStateKey> block_state_by_id(int32_t id) {
  static const std::unordered_map<int32_t, BlockStateKey> table = [] {
    std::unordered_map<int32_t, BlockStateKey> by_id;
    by_id.reserve(kBlockStateCount);
    for (const GeneratedBlockState& entry : kBlockStates)
      by_id.emplace(entry.id, BlockStateKey{std::string(entry.name), std::string(entry.props)});
    return by_id;
  }();
  const auto it = table.find(id);
  if (it == table.end())
    return std::nullopt;
  return it->second;
}

const BlockStateTable& observed_block_states() {
  // kObservedBlockStates and kObservedBlockStateIds are emitted in the same
  // (id, name, props) sort order, so index i of one corresponds to index i of
  // the other (see tools/map_states.py). The live-overworld capture never
  // cell-aligned on a bedrock cell (the spawn column's Y=-4 section drifted),
  // so fold the milestone-A hand table in too — it is byte-verified bedrock
  // ground truth from the flat fixture.
  static const StaticBlockStateTable table = [] {
    std::vector<std::pair<BlockStateKey, int32_t>> states;
    states.reserve(kObservedBlockStateCount + 4);
    const auto& extra = milestone_a_entries();
    // Observed ids win (same values for overlapping states anyway); the
    // milestone-A entries add byte-verified states the live capture never
    // cell-aligned on (bedrock), as long as they are not duplicates.
    for (int i = 0; i < kObservedBlockStateCount; ++i) {
      BlockStateKey key;
      key.name = kObservedBlockStates[i].first;
      key.properties = kObservedBlockStates[i].second;
      states.emplace_back(std::move(key), kObservedBlockStateIds[i]);
    }
    for (const auto& e : extra.entries()) {
      bool dup = false;
      for (const auto& [k, v] : states)
        if (k == e.first) {
          dup = true;
          break;
        }
      if (!dup)
        states.emplace_back(e);
    }
    return StaticBlockStateTable(std::move(states));
  }();
  return table;
}

std::optional<DefaultBlockState> default_block_state(std::string_view name) {
  // name -> (props, id), built once. The table is one entry per block (1196
  // in 26.2), so a flat hash map is plenty.
  static const std::unordered_map<std::string, const GeneratedDefaultBlockState*> index = [] {
    std::unordered_map<std::string, const GeneratedDefaultBlockState*> m;
    m.reserve(kDefaultBlockStateCount * 2);
    for (const GeneratedDefaultBlockState& e : kDefaultBlockStates)
      m.emplace(e.name, &e);
    return m;
  }();
  std::string key(name);
  // Accept the bare forms callers use ("stone", "minecraft:stone").
  if (key.find(':') == std::string::npos)
    key.insert(0, "minecraft:");
  const auto it = index.find(key);
  if (it == index.end())
    return std::nullopt;
  DefaultBlockState out;
  out.key.name = it->second->name;
  out.key.properties = it->second->props;
  out.id = it->second->id;
  return out;
}

std::optional<int32_t> NameListBiomeTable::lookup(const std::string& name) const {
  const auto it = index_.find(name);
  if (it == index_.end())
    return std::nullopt;
  return it->second;
}

} // namespace mc::world
