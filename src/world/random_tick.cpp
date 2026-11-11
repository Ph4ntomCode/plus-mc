// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/random_tick.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_map>

#include "world/crop.h"
#include "world/leaves.h"

namespace mc::world {
namespace {

// The generated table, indexed by name once: a cold chunk load asks this for
// every palette entry of every section (a section's distinct states, ~184 on the
// ocean fixture), and a binary search of ~500 long block names per ask was the
// largest single item left in the parse. The map is read-only after its
// thread-safe first use, and its keys point into the generated table's own
// string literals, so it allocates nothing per lookup.
const GeneratedRandomTickBlock* find_block(std::string_view name) {
  static const std::unordered_map<std::string_view, const GeneratedRandomTickBlock*> by_name = [] {
    std::unordered_map<std::string_view, const GeneratedRandomTickBlock*> out;
    out.reserve(static_cast<size_t>(kRandomTickBlockCount) * 2);
    for (const GeneratedRandomTickBlock* entry = kRandomTickBlocks;
         entry != kRandomTickBlocks + kRandomTickBlockCount; ++entry)
      out.emplace(std::string_view(entry->name), entry);
    return out;
  }();
  const auto it = by_name.find(name);
  return it == by_name.end() ? nullptr : it->second;
}

// The bounds of `key`'s value inside a canonical properties string, or
// nullopt. `key` only ever matches at the start of the string or right after a
// comma, so `age` cannot match inside `stage`.
std::optional<std::pair<size_t, size_t>> find_value(const std::string& properties,
                                                    std::string_view key) {
  size_t pos = 0;
  while (pos <= properties.size()) {
    const size_t comma = properties.find(',', pos);
    const size_t end = comma == std::string::npos ? properties.size() : comma;
    const size_t eq = properties.find('=', pos);
    if (eq != std::string::npos && eq < end && eq - pos == key.size() &&
        properties.compare(pos, key.size(), key) == 0)
      return std::make_pair(eq + 1, end);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }
  return std::nullopt;
}

} // namespace

std::optional<int> state_prop_int(const std::string& properties, std::string_view key) {
  const std::optional<std::pair<size_t, size_t>> value = find_value(properties, key);
  if (!value)
    return std::nullopt;
  const size_t begin = value->first;
  const size_t end = value->second;
  if (begin >= end)
    return std::nullopt;
  // Parsed in place. The `substr` this used to build was a heap allocation for
  // every crop's `age` read and every leaf's `distance` read — the random-tick
  // pass asks these per sampled cell, so the allocation showed up in `sample`
  // as `basic_string` churn rather than as parsing. Overflow is refused rather
  // than wrapped (the old `atoi` was undefined there); every vanilla property
  // value is a small integer, so the answers are identical.
  int result = 0;
  for (size_t i = begin; i < end; ++i) {
    const unsigned char c = static_cast<unsigned char>(properties[i]);
    if (c < '0' || c > '9')
      return std::nullopt;
    const int digit = c - '0';
    if (result > (std::numeric_limits<int>::max() - digit) / 10)
      return std::nullopt;
    result = result * 10 + digit;
  }
  return result;
}

std::optional<bool> state_prop_bool(const std::string& properties, std::string_view key) {
  const std::optional<std::pair<size_t, size_t>> value = find_value(properties, key);
  if (!value)
    return std::nullopt;
  const std::string_view text(properties.data() + value->first, value->second - value->first);
  if (text == "true")
    return true;
  if (text == "false")
    return false;
  return std::nullopt;
}

BlockStateKey state_with_prop_value(const BlockStateKey& state, std::string_view key,
                                    std::string_view value) {
  const std::optional<std::pair<size_t, size_t>> found = find_value(state.properties, key);
  if (!found)
    return state;
  BlockStateKey out = state;
  out.properties.replace(found->first, found->second - found->first, value);
  return out;
}

BlockStateKey state_with_prop_int(const BlockStateKey& state, std::string_view key, int value) {
  // The value is written into a stack buffer rather than `std::to_string`'s
  // string: the crop and leaf ticks rebuild the same state once per cell, and
  // each one was an allocation to format two or three digits.
  char text[16];
  const auto written = std::to_chars(text, text + sizeof(text), value);
  return state_with_prop_value(state, key,
                               std::string_view(text, static_cast<size_t>(written.ptr - text)));
}

RandomTickKind random_tick_kind(std::string_view block_name) {
  const GeneratedRandomTickBlock* entry = find_block(block_name);
  return entry == nullptr ? kRandomTickNone : static_cast<RandomTickKind>(entry->kind);
}

bool block_is_randomly_ticking(const BlockStateKey& state) {
  switch (random_tick_kind(state.name)) {
  case kRandomTickNone:
    return false;
  case kRandomTickCrop:
    return crop_is_randomly_ticking(state);
  case kRandomTickLeaves:
    return leaves_is_randomly_ticking(state);
  case kRandomTickOther:
    return true;
  }
  return false;
}

void tick_chunk_random_blocks(const RandomTickLevel& level, const RandomTickRandom& rng,
                              BlockRandomPos& rand, int tick_speed, int chunk_x, int chunk_z) {
  if (tick_speed <= 0 || !level.ticking_sections || !level.get_block)
    return;
  const uint64_t mask = level.ticking_sections(chunk_x, chunk_z);
  if (mask == 0)
    return;
  const int min_x = chunk_x * 16;
  const int min_z = chunk_z * 16;
  const int sections = std::min(level.section_count, 64);
  for (int index = 0; index < sections; ++index) {
    if ((mask & (1ull << index)) == 0)
      continue;
    const int min_y = (level.min_section_y + index) * 16;
    for (int i = 0; i < tick_speed; ++i) {
      int x = 0, y = 0, z = 0;
      // `Level.getBlockRandomPos(minX, minYInSection, minZ, 15)`.
      rand.next(min_x, min_y, min_z, 15, &x, &y, &z);
      const BlockStateKey state = level.get_block(x, y, z);
      if (!block_is_randomly_ticking(state))
        continue;
      random_tick_block(level, rng, state, x, y, z);
    }
  }
}

bool random_tick_block(const RandomTickLevel& level, const RandomTickRandom& rng,
                       const BlockStateKey& state, int x, int y, int z) {
  switch (random_tick_kind(state.name)) {
  case kRandomTickCrop:
    return crop_random_tick(level, rng, state, x, y, z);
  case kRandomTickLeaves:
    return leaves_random_tick(level, rng, state, x, y, z);
  case kRandomTickNone:
  case kRandomTickOther:
    break;
  }
  return false;
}

} // namespace mc::world
