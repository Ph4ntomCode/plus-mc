// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/trim.h"

namespace mc::world {

int32_t trim_material_for_item(int32_t item) {
  if (item <= 0)
    return -1;
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i)
    if (kTrimMaterials[i].item == item)
      return kTrimMaterials[i].id;
  return -1;
}

int32_t trim_pattern_for_template(int32_t item) {
  if (item <= 0)
    return -1;
  for (uint32_t i = 0; i < kTrimPatternCount; ++i)
    if (kTrimPatterns[i].template_item == item)
      return kTrimPatterns[i].id;
  return -1;
}

// Both tables are ordered by id (the generated header's contract), so a name
// lookup is a linear scan and an id lookup is the index itself — but the scan
// is what keeps a hand-written id honest, and it costs nothing at this size.
const char* trim_material_name(int32_t id) {
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i)
    if (kTrimMaterials[i].id == id)
      return kTrimMaterials[i].name;
  return nullptr;
}

const char* trim_pattern_name(int32_t id) {
  for (uint32_t i = 0; i < kTrimPatternCount; ++i)
    if (kTrimPatterns[i].id == id)
      return kTrimPatterns[i].name;
  return nullptr;
}

int32_t trim_material_for_name(std::string_view name) {
  if (name.empty())
    return -1;
  for (uint32_t i = 0; i < kTrimMaterialCount; ++i)
    if (name == kTrimMaterials[i].name)
      return kTrimMaterials[i].id;
  return -1;
}

int32_t trim_pattern_for_name(std::string_view name) {
  if (name.empty())
    return -1;
  for (uint32_t i = 0; i < kTrimPatternCount; ++i)
    if (name == kTrimPatterns[i].name)
      return kTrimPatterns[i].id;
  return -1;
}

} // namespace mc::world
