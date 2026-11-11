// SPDX-License-Identifier: GPL-3.0-or-later
//
// The entity slicer the region tick path puts on top of the scheduler
// (src/sim/region_entities.h, docs/PHASE5.md ordered item 3): the properties
// the orb, item, potion, projectile and TNT passes rely on —
//
//   1. **exactly once.** Every entity lands in exactly one slice, and the slots
//      cover the population.
//   2. **closed under coupling.** A pair the caller unioned is never split by a
//      region edge; the cluster lands in the region of its lowest-indexed
//      member.
//   3. **canonical order.** The slice list is the scheduler's `(rx, rz)` order,
//      so the merge's packet order is a property of the world rather than of a
//      lane.
//   4. **restore is the identity** for a tick that removed nothing, and drops
//      exactly what the tick erased — in the population's original order, so a
//      later walk of the registry does not see a region-grouped one.
//
// A slicer that lost an entity, split a cluster, or restored slices in region
// order would pass a compile and fail here.
//
// Usage: region_entities_test

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "sim/region_entities.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                         \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

using mc::sim::EntityClusters;
using mc::sim::EntitySlicePlan;
using mc::sim::RegionGrid;
using mc::sim::RegionPos;

// A stand-in for any of the registries' entities: an id, a block position and a
// removal flag the test's "tick" can set.
struct Fake {
  int32_t id = 0;
  int x = 0;
  int z = 0;
  bool removed = false;
};

int32_t id_of(const Fake& fake) {
  return fake.id;
}

std::pair<int, int> chunk_of(const Fake& fake) {
  return {fake.x >> 4, fake.z >> 4};
}

// A tick that erases the flagged entries and leaves the rest in order — the
// shape `tick_orbs`, `tick_item_entities` and friends have.
void erase_removed(std::vector<Fake>& entities) {
  for (auto it = entities.begin(); it != entities.end();) {
    if (it->removed)
      it = entities.erase(it);
    else
      ++it;
  }
}

void check_clusters() {
  EntityClusters clusters(6);
  CHECK(clusters.count() == 6);
  for (size_t i = 0; i < 6; ++i)
    CHECK(clusters.root(i) == i);

  // The root is the lowest index, whichever way the pair is written.
  clusters.join(3, 5);
  CHECK(clusters.root(3) == 3);
  CHECK(clusters.root(5) == 3);
  clusters.join(4, 3);
  CHECK(clusters.root(3) == 3);
  CHECK(clusters.root(4) == 3);
  CHECK(clusters.root(5) == 3);
  // Joining an already-joined pair does not move the root.
  clusters.join(5, 4);
  CHECK(clusters.root(4) == 3);
  // The untouched index is still alone.
  CHECK(clusters.root(2) == 2);
}

void check_partition_and_restore() {
  const RegionGrid grid(1); // one chunk a side: every 16-block cell is a region
  std::vector<Fake> entities;
  // A 6x6 grid of ids at four-block spacing: every chunk holds four, so the
  // population spans four regions and the test crosses edges in both axes.
  int32_t next_id = 100;
  for (int x = 0; x < 24; x += 4) {
    for (int z = 0; z < 24; z += 4) {
      Fake fake;
      fake.id = next_id++;
      fake.x = x;
      fake.z = z;
      entities.push_back(fake);
    }
  }
  std::vector<Fake> original = entities;
  const size_t population = entities.size();

  // Couple two entities that sit either side of a region edge. The scan below
  // finds one the grid itself puts in two regions, rather than assuming which
  // coordinate the edge is at.
  const auto index_at = [&entities](int x, int z) {
    for (size_t i = 0; i < entities.size(); ++i)
      if (entities[i].x == x && entities[i].z == z)
        return i;
    return entities.size();
  };
  // Find a pair the grid puts in different regions but that are neighbours.
  size_t a = entities.size(), b = entities.size();
  RegionPos region_a{}, region_b{};
  for (size_t i = 0; i < entities.size() && b == entities.size(); ++i) {
    const RegionPos ra = grid.region_of(entities[i].x >> 4, entities[i].z >> 4);
    for (size_t j = i + 1; j < entities.size(); ++j) {
      const RegionPos rb = grid.region_of(entities[j].x >> 4, entities[j].z >> 4);
      if (ra != rb && entities[j].x == entities[i].x + 4 && entities[j].z == entities[i].z) {
        a = i;
        b = j;
        region_a = ra;
        region_b = rb;
        break;
      }
    }
  }
  CHECK(a != entities.size());
  CHECK(b != entities.size());
  CHECK(!(region_a == region_b));

  EntityClusters clusters(population);
  clusters.join(b, a); // join the higher index to the lower one
  // And a second cluster entirely inside one region, to show the slicer does
  // not split what it does not have to.
  const size_t c = index_at(0, 0);
  const size_t d = index_at(0, 4);
  CHECK(c != entities.size());
  CHECK(d != entities.size());
  clusters.join(c, d);

  EntitySlicePlan<Fake> plan =
      mc::sim::make_entity_slice_plan(entities, grid, clusters, id_of, chunk_of);
  CHECK(entities.empty());
  CHECK(plan.population == population);
  CHECK(plan.regions.size() == plan.slices.size());
  CHECK(!plan.regions.empty());

  // Canonical order and every entity exactly once.
  size_t seen = 0;
  std::vector<int> appearances(population, 0);
  for (size_t s = 0; s < plan.slices.size(); ++s) {
    CHECK(plan.slices[s].region == plan.regions[s]);
    if (s > 0)
      CHECK(plan.regions[s - 1] < plan.regions[s]);
    CHECK(!plan.slices[s].slots.empty());
    CHECK(plan.slices[s].slots.size() == plan.slices[s].ids.size());
    CHECK(plan.slices[s].slots.size() == plan.slices[s].entities.size());
    for (size_t k = 0; k < plan.slices[s].slots.size(); ++k) {
      CHECK(plan.slices[s].slots[k] < population);
      ++appearances[plan.slices[s].slots[k]];
      ++seen;
      // The ids travel with the entities.
      CHECK(plan.slices[s].ids[k] == original[plan.slices[s].slots[k]].id);
      CHECK(plan.slices[s].entities[k].id == plan.slices[s].ids[k]);
    }
    // Slots ascend inside a slice.
    for (size_t k = 1; k < plan.slices[s].slots.size(); ++k)
      CHECK(plan.slices[s].slots[k - 1] < plan.slices[s].slots[k]);
  }
  CHECK(seen == population);
  for (int count : appearances)
    CHECK(count == 1);

  // Closed under coupling: the cross-edge pair is in one slice, and that slice
  // is the region of the pair's lower index (`a`).
  size_t slice_a = plan.slices.size(), slice_b = plan.slices.size();
  for (size_t s = 0; s < plan.slices.size(); ++s)
    for (const size_t slot : plan.slices[s].slots) {
      if (slot == a)
        slice_a = s;
      if (slot == b)
        slice_b = s;
    }
  CHECK(slice_a != plan.slices.size());
  CHECK(slice_a == slice_b);
  CHECK(plan.slices[slice_a].region == region_a);
  // ...and a cluster that was already inside one region stayed there.
  for (size_t s = 0; s < plan.slices.size(); ++s) {
    bool has_c = false, has_d = false;
    for (const size_t slot : plan.slices[s].slots) {
      has_c = has_c || slot == c;
      has_d = has_d || slot == d;
    }
    if (has_c || has_d)
      CHECK(has_c && has_d);
  }

  // `find` answers the same slice the order implies, and nothing for a region
  // the tick does not cover.
  for (size_t s = 0; s < plan.regions.size(); ++s) {
    const auto* found = plan.find(plan.regions[s]);
    CHECK(found != nullptr);
    if (found != nullptr)
      CHECK(found->region == plan.regions[s]);
  }
  CHECK(plan.find(RegionPos{99, 99}) == nullptr);

  // Restore with nothing removed is the identity — same entities, same order.
  mc::sim::restore_entity_slices(entities, plan, id_of);
  CHECK(entities.size() == population);
  for (size_t i = 0; i < population && i < entities.size(); ++i)
    CHECK(entities[i].id == original[i].id);
  CHECK(entities.size() == original.size());
  for (size_t i = 0; i < population && i < entities.size(); ++i) {
    CHECK(entities[i].x == original[i].x);
    CHECK(entities[i].z == original[i].z);
  }
}

void check_restore_drops_the_erased() {
  const RegionGrid grid(1);
  std::vector<Fake> entities;
  int32_t next_id = 1;
  for (int x = 0; x < 32; x += 4)
    for (int z = 0; z < 32; z += 4) {
      Fake fake;
      fake.id = next_id++;
      fake.x = x;
      fake.z = z;
      entities.push_back(fake);
    }
  std::vector<Fake> original = entities;
  const size_t population = entities.size();
  EntityClusters clusters(population);
  EntitySlicePlan<Fake> plan =
      mc::sim::make_entity_slice_plan(entities, grid, clusters, id_of, chunk_of);
  CHECK(entities.empty());
  CHECK(plan.population == population);

  // Mark one entity per slice — including the first and the last of a slice —
  // and run the erase-shaped tick on every slice, as a lane would.
  std::vector<bool> removed(population, false);
  for (size_t s = 0; s < plan.slices.size(); ++s) {
    const size_t last = plan.slices[s].entities.size() - 1;
    for (size_t k : {size_t{0}, last}) {
      plan.slices[s].entities[k].removed = true;
      removed[plan.slices[s].slots[k]] = true;
    }
    erase_removed(plan.slices[s].entities);
  }

  mc::sim::restore_entity_slices(entities, plan, id_of);
  size_t expected = 0;
  for (size_t i = 0; i < population; ++i)
    if (!removed[i])
      ++expected;
  CHECK(entities.size() == expected);
  size_t at = 0;
  for (size_t i = 0; i < population && at < entities.size(); ++i) {
    if (removed[i])
      continue;
    CHECK(entities[at].id == original[i].id);
    CHECK(entities[at].x == original[i].x);
    CHECK(entities[at].z == original[i].z);
    ++at;
  }
  CHECK(at == expected);
}

void check_empty_and_one_region() {
  const RegionGrid grid(8);
  {
    std::vector<Fake> empty;
    EntityClusters clusters(0);
    EntitySlicePlan<Fake> plan =
        mc::sim::make_entity_slice_plan(empty, grid, clusters, id_of, chunk_of);
    CHECK(plan.population == 0);
    CHECK(plan.regions.empty());
    CHECK(plan.slices.empty());
    mc::sim::restore_entity_slices(empty, plan, id_of);
    CHECK(empty.empty());
  }
  {
    // Every entity in one chunk: one region, one slice, and the cluster order
    // is the population order.
    std::vector<Fake> entities{{7, 1, 1, false}, {5, 2, 3, false}, {9, 3, 2, false}};
    EntityClusters clusters(entities.size());
    clusters.join(2, 0);
    EntitySlicePlan<Fake> plan =
        mc::sim::make_entity_slice_plan(entities, grid, clusters, id_of, chunk_of);
    CHECK(plan.regions.size() == 1);
    CHECK(plan.regions[0] == (RegionPos{0, 0}));
    CHECK(plan.slices[0].slots == (std::vector<size_t>{0, 1, 2}));
    CHECK(plan.slices[0].ids == (std::vector<int32_t>{7, 5, 9}));
    // The cluster's region is member 0's, which is this chunk anyway.
    mc::sim::restore_entity_slices(entities, plan, id_of);
    CHECK(entities.size() == 3);
    CHECK(entities[0].id == 7);
    CHECK(entities[1].id == 5);
    CHECK(entities[2].id == 9);
  }
}

} // namespace

int main() {
  std::printf("region_entities_test\n");
  check_clusters();
  check_partition_and_restore();
  check_restore_drops_the_erased();
  check_empty_and_one_region();
  if (failures != 0) {
    std::fprintf(stderr, "region_entities_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("region_entities_test: all checks passed\n");
  return 0;
}
