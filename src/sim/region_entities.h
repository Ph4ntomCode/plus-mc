// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entity-population slicing for the region scheduler (docs/PLAN.md §6.2,
// docs/PHASE5.md ordered item 3). The scheduler owns *policy* — regions, lanes,
// seeds — and the mob pass in server/session.cpp showed the shape an entity
// population needs on top of it:
//
//   1. decide which entities must stay together (a mob with its mate or parent),
//   2. place each cluster in the region of its lowest-indexed member,
//   3. move each region's slice out of the registry and hand it to one lane,
//   4. put the survivors back at their original slots, so the registry's order
//      after the tick is the order it had before it.
//
// This header is steps 2-4 for any population, so the orb, item, potion,
// projectile and TNT passes do not each re-derive the grouping and the restore.
// Steps 1 and 3's body stay with the caller: only it knows what a slice reads.
//
// The slice is *closed* under its coupling by construction: a cluster is never
// split by a region edge, so a tick that reads another member of its own slice
// (an orb merging into a neighbour, an item absorbing a stack) sees the same
// entities whichever lane runs it, and no lane ever looks at another lane's
// live entity.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "sim/region_scheduler.h"

namespace mc::sim {

// The union-find the caller fills with the pairs its tick reads across. The
// representative is always the cluster's lowest index, so the region a cluster
// lands in does not depend on the order the links were seen in.
class EntityClusters {
public:
  explicit EntityClusters(size_t count) : parent_(count) {
    for (size_t i = 0; i < count; ++i)
      parent_[i] = i;
  }

  size_t count() const {
    return parent_.size();
  }

  // Path halving. The tree is a link deep at most, because `join` always hangs
  // the higher root under the lower one.
  size_t root(size_t index) {
    while (parent_[index] != index) {
      parent_[index] = parent_[parent_[index]];
      index = parent_[index];
    }
    return index;
  }

  void join(size_t a, size_t b) {
    const size_t ra = root(a), rb = root(b);
    if (ra == rb)
      return;
    if (ra < rb)
      parent_[rb] = ra;
    else
      parent_[ra] = rb;
  }

private:
  std::vector<size_t> parent_;
};

// One region's slice: the entities it owns for the tick, the slots they came
// from (ascending) and their entity ids at tick start.
template <typename Entity> struct EntitySlice {
  RegionPos region;
  std::vector<size_t> slots;
  std::vector<int32_t> ids;
  std::vector<Entity> entities;
};

// The whole partition: `regions` is the scheduler's canonical order and
// `slices[i]` is `regions[i]`'s slice.
template <typename Entity> struct EntitySlicePlan {
  size_t population = 0;
  std::vector<RegionPos> regions;
  std::vector<EntitySlice<Entity>> slices;

  const EntitySlice<Entity>* find(RegionPos region) const {
    const auto it = std::lower_bound(regions.begin(), regions.end(), region);
    if (it == regions.end() || !(*it == region))
      return nullptr;
    return &slices[static_cast<size_t>(it - regions.begin())];
  }

  EntitySlice<Entity>* find(RegionPos region) {
    const auto it = std::lower_bound(regions.begin(), regions.end(), region);
    if (it == regions.end() || !(*it == region))
      return nullptr;
    return &slices[static_cast<size_t>(it - regions.begin())];
  }
};

// Partitions `entities` (which is left empty) into one slice per tick region.
// `clusters` must have been filled by the caller over the same indices, and
// `chunk_of` answers the chunk an entity occupies *at tick start* — the slice a
// cluster lands in is the region of that chunk for its lowest-indexed member.
template <typename Entity, typename IdOf, typename ChunkOf>
EntitySlicePlan<Entity> make_entity_slice_plan(std::vector<Entity>& entities,
                                               const RegionGrid& grid, EntityClusters& clusters,
                                               IdOf id_of, ChunkOf chunk_of) {
  EntitySlicePlan<Entity> plan;
  plan.population = entities.size();
  if (entities.empty())
    return plan;

  // A cluster's region is decided by its first member in index order, which is
  // its root (the root is the lowest index).
  std::vector<std::optional<RegionPos>> cluster_region(clusters.count());
  std::map<RegionPos, std::vector<size_t>> groups;
  for (size_t i = 0; i < entities.size(); ++i) {
    const size_t root = clusters.root(i);
    std::optional<RegionPos>& region = cluster_region[root];
    if (!region) {
      const std::pair<int, int> chunk = chunk_of(entities[i]);
      region = grid.region_of(chunk.first, chunk.second);
    }
    groups[*region].push_back(i);
  }

  plan.regions.reserve(groups.size());
  plan.slices.reserve(groups.size());
  for (auto& group : groups) {
    EntitySlice<Entity> slice;
    slice.region = group.first;
    slice.slots = std::move(group.second);
    slice.ids.reserve(slice.slots.size());
    for (const size_t slot : slice.slots)
      slice.ids.push_back(id_of(entities[slot]));
    slice.entities.reserve(slice.slots.size());
    for (const size_t slot : slice.slots)
      slice.entities.push_back(std::move(entities[slot]));
    plan.regions.push_back(slice.region);
    plan.slices.push_back(std::move(slice));
  }
  entities.clear();
  return plan;
}

// Puts the survivors back at their original slots, in the population's original
// order; whatever a slice's tick erased simply disappears. The walk assumes the
// tick preserved the order of the survivors and never reordered them — true of
// every `ticks_*` module, which erases and reports rather than sorting. A slice
// that somehow lost an id the module did not report as removed is skipped
// rather than mis-assigned: the ids are matched, not counted.
template <typename Entity, typename IdOf>
void restore_entity_slices(std::vector<Entity>& entities, EntitySlicePlan<Entity>& plan,
                           IdOf id_of) {
  std::vector<std::optional<Entity>> placed(plan.population);
  for (EntitySlice<Entity>& slice : plan.slices) {
    size_t survivor = 0;
    for (size_t k = 0; k < slice.ids.size() && survivor < slice.entities.size(); ++k) {
      if (id_of(slice.entities[survivor]) != slice.ids[k])
        continue;
      placed[slice.slots[k]] = std::move(slice.entities[survivor]);
      ++survivor;
    }
  }
  entities.clear();
  entities.reserve(plan.population);
  for (std::optional<Entity>& entity : placed)
    if (entity)
      entities.push_back(std::move(*entity));
}

} // namespace mc::sim
