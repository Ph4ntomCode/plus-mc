// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pathfinder's answer contract, pinned as a digest.
//
// `src/world/pathfinding.cpp` has been optimised twice since it landed (one node
// table instead of two, a thread-local scratch, a one-probe block cache), and
// each time the argument that it was still correct rested on a private probe:
// the same corpus run against the old and the new translation unit, with a
// digest of every answer. That probe is this file. It is checked in so the
// argument is a `ctest` target instead of a `/tmp` directory:
//
//   ctest -R pathfinding-digest
//
// What it pins: for each case, `nav_nearest_walkable(to, env, 4)` (its cell and
// its `blocked` flag), `nav_cell_walkable` on the case's cells, and then
// `find_path(from, to, env)` — the `reached` flag, the path length, and every
// waypoint, mixed into an FNV-1a digest. A change that alters any answer changes
// the digest, and the gate fails.
//
// The corpus is deterministic and needs no worldgen: a seeded random family
// (walls, pillars, floor holes) plus small families built for the shapes the
// search has special rules for — diagonal corner cuts (including the second,
// head-height half of the test), headroom (a 0.7-tall chicken, a 1.95 zombie and
// a 2.6-tall golem through the same 1- and 2-block ceilings), ledges (a step up,
// 1-3 block drops and a fall past `kNavMaxFall`), goals that cannot be reached
// (partial paths, and the bounded search with a small `max_nodes`) and goals
// buried in blocks (`nav_nearest_walkable`'s own search, including the shell
// that finds nothing).
//
// To re-pin after a *deliberate* behaviour change, run the binary with `--print`
// and put the printed aggregate digest in `kExpectedDigest`, saying why in the
// commit. A re-pin is the acknowledgement that path shapes moved.
//
// Determinism note: the digest mixes integers only, and the search's own
// decisions were measured to be stable across -O0/-O2/-O3 and both
// `-ffp-contract` settings (all five read the same digest), so the constant is
// not expected to be compiler-dependent. If a toolchain ever disagrees, fix the
// corpus rather than pinning a second number.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>
#include <vector>

#include "world/pathfinding.h"

using namespace mc::world;

namespace {

// The pinned aggregate, computed from the corpus below (see the header). It was
// first taken from the working tree, and the same corpus read the same value
// against the pre-optimisation translation unit (HEAD's pathfinding.cpp), which
// is the whole claim: the optimised search answers what the simple one did.
constexpr uint64_t kExpectedDigest = 0x25181A45E7DF2AABull;

// FNV-1a: the same mix the private differential probe used, so the numbers in
// the records (e.g. the random family's 8fa1dbc7b6d10afa at 20 x 30 x 3) can be
// checked against this file.
struct Digest {
  uint64_t h = 1469598103934665603ull;
  void mix(uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  }
  void mix_i(int v) {
    mix(static_cast<uint64_t>(static_cast<int64_t>(v)));
  }
};

// The probe's generator: xorshift64, seed 0x9e3779b97f4a7c15.
struct Rng {
  uint64_t s = 0x9e3779b97f4a7c15ull;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  int range(int n) {
    return static_cast<int>(next() % static_cast<uint64_t>(n));
  }
};

struct World {
  std::set<std::tuple<int, int, int>> blocks;
  bool floor = true;

  NavEnvironment env(double height) const {
    NavEnvironment out;
    out.height = height;
    out.is_solid = [this](int x, int y, int z) {
      if (floor && y <= 0)
        return true;
      return blocks.count(std::make_tuple(x, y, z)) != 0;
    };
    return out;
  }
};

NavCell cell(int x, int y, int z) {
  return NavCell{x, y, z};
}

struct Tally {
  long searches = 0;
  long reached = 0;
  long cells = 0;
  long blocked = 0;
};

// One case, digested in the probe's order: the nearest-walkable answer, then the
// path's. `radius` and `max_nodes` are exposed so the bounded-search family can
// shrink the budget. Kept exactly as the probe mixed it -- adding a mix in here
// would move the random family's digest off the 8fa1dbc7b6d10afa its records
// quote, which is the check that this file's corpus really is that corpus.
void digest_case(Digest& digest, Tally& tally, const NavEnvironment& env, const NavCell& from,
                 const NavCell& to, int radius = 4, int max_nodes = kNavMaxNodes) {
  bool blocked = false;
  const NavCell near = nav_nearest_walkable(to, env, radius, &blocked);
  digest.mix_i(blocked ? 1 : 0);
  digest.mix_i(near.x);
  digest.mix_i(near.y);
  digest.mix_i(near.z);
  tally.blocked += blocked ? 1 : 0;

  bool reached = false;
  const NavPath path = find_path(from, to, env, &reached, max_nodes);
  ++tally.searches;
  tally.reached += reached ? 1 : 0;
  tally.cells += static_cast<long>(path.points.size());
  digest.mix_i(reached ? 1 : 0);
  digest.mix(static_cast<uint64_t>(path.points.size()));
  for (const NavCell& c : path.points) {
    digest.mix_i(c.x);
    digest.mix_i(c.y);
    digest.mix_i(c.z);
  }
}

// `nav_cell_walkable` on its own: the public one-cell question, mixed with the
// cell's own coordinates so a case can ask about a specific shape (a roofed
// cell, a head-height block, a cell inside the rock) without inventing a path
// pair for it. Used by the targeted families only, never by the random one.
void digest_cell(Digest& digest, const NavEnvironment& env, const NavCell& c) {
  digest.mix_i(c.x);
  digest.mix_i(c.y);
  digest.mix_i(c.z);
  digest.mix_i(nav_cell_walkable(c, env) ? 1 : 0);
}

struct Family {
  const char* name;
  const char* shape;
  Digest digest;
  Tally tally;
};

const double kHeights[3] = {0.7, 1.95, 2.6};

// --- the random family: the private probe's own corpus ------------------------
// 20 worlds x 3 heights x 30 pairs, the generator and the case order of the
// probe, so the family digest is the number its records quote
// (8fa1dbc7b6d10afa) -- a self-check on this file's port of the corpus.
void random_family(Family& family) {
  constexpr int kWorlds = 20;
  constexpr int kPairs = 30;
  Rng rng;
  for (int w = 0; w < kWorlds; ++w) {
    World world;
    for (int i = 0; i < 24; ++i) {
      const int x = rng.range(41) - 20;
      const int z = rng.range(41) - 20;
      const int h = 1 + rng.range(3);
      const int len = 1 + rng.range(6);
      const bool along_x = (rng.range(2) == 0);
      for (int k = 0; k < len; ++k)
        for (int y = 1; y <= h; ++y) {
          const int bx = along_x ? x + k : x;
          const int bz = along_x ? z : z + k;
          world.blocks.emplace(bx, y, bz);
        }
      if (rng.range(3) == 0) { // a floor hole (a fall trap)
        const int bx = rng.range(41) - 20;
        const int bz = rng.range(41) - 20;
        world.blocks.emplace(bx, 0, bz);
      }
    }
    for (double height : kHeights) {
      const NavEnvironment env = world.env(height);
      for (int p = 0; p < kPairs; ++p) {
        const NavCell from{rng.range(41) - 20, 1, rng.range(41) - 20};
        const NavCell to{rng.range(41) - 20, 1, rng.range(41) - 20};
        digest_case(family.digest, family.tally, env, from, to);
      }
    }
  }
}

// --- corner cuts -------------------------------------------------------------
// `step_to` refuses a diagonal whose two flanking cells are blocked at the mob's
// own level or the one above. Four orientations, each with the flank blocked and
// then with only the flank's *head-height* cell blocked (the second half of the
// test), plus the same corner left open as the control.
void corner_family(Family& family) {
  for (int dz = -1; dz <= 1; dz += 2) {
    for (int dx = -1; dx <= 1; dx += 2) {
      for (int variant = 0; variant < 3; ++variant) {
        World world;
        if (variant == 0) { // both flanks blocked at foot level: the corner is shut
          world.blocks.emplace(2, 1, 3);
          world.blocks.emplace(3, 1, 2);
        } else if (variant == 1) { // only at head height
          world.blocks.emplace(2, 2, 3);
          world.blocks.emplace(3, 2, 2);
        }
        for (double height : kHeights) {
          const NavEnvironment env = world.env(height);
          // The two flanking cells, the corner itself and the far side.
          digest_cell(family.digest, env, cell(2, 1, 3));
          digest_cell(family.digest, env, cell(2, 2, 3));
          digest_cell(family.digest, env, cell(3, 1, 2));
          // Walk diagonally into the corner, and out of it again.
          digest_case(family.digest, family.tally, env, cell(2, 1, 2), cell(3, 1, 3));
          digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(5 + dx, 1, 5 + dz));
        }
      }
    }
  }
}

// --- headroom ----------------------------------------------------------------
// One corridor roofed at y=2 (a single clear block: a chicken's cell, not a
// zombie's) and one roofed at y=3 (two clear blocks: both fit), with pairs along
// each and a pair that has to leave the low corridor.
void headroom_family(Family& family) {
  World world;
  for (int x = 2; x <= 10; ++x) {
    world.blocks.emplace(x, 2, 0); // roof: 1 block of clearance above the floor
    world.blocks.emplace(x, 3, 4); // roof: 2 blocks of clearance
  }
  for (double height : kHeights) {
    const NavEnvironment env = world.env(height);
    digest_cell(family.digest, env, cell(6, 1, 0)); // one clear block above it
    digest_cell(family.digest, env, cell(6, 2, 0)); // the roof itself
    digest_cell(family.digest, env, cell(6, 1, 4)); // two clear blocks above it
    digest_case(family.digest, family.tally, env, cell(1, 1, 0), cell(11, 1, 0)); // low corridor
    digest_case(family.digest, family.tally, env, cell(1, 1, 4), cell(11, 1, 4)); // high corridor
    digest_case(family.digest, family.tally, env, cell(1, 1, 0), cell(11, 1, 4)); // out and around
    digest_case(family.digest, family.tally, env, cell(6, 1, 0),
                cell(6, 1, 2)); // out of the low one
  }
}

// --- ledges ------------------------------------------------------------------
// A step up, drops of one, two and three blocks (the allowed fall distances), a
// four-block drop (past `kNavMaxFall`: the ping-pong one is a wall, not a step
// down), and a ledge whose only way on is back up the way it came.
void ledge_family(Family& family) {
  World world;
  // x 2..4: one block up (cells at y=2); x 5..7: two up; x 8..10: three up.
  for (int x = 2; x <= 4; ++x)
    world.blocks.emplace(x, 1, 0);
  for (int x = 5; x <= 7; ++x)
    for (int y = 1; y <= 2; ++y)
      world.blocks.emplace(x, y, 0);
  for (int x = 8; x <= 10; ++x)
    for (int y = 1; y <= 3; ++y)
      world.blocks.emplace(x, y, 0);
  // x 2..10 at z=8: a shelf at y=5, four blocks above the floor: no fall from
  // the floor reaches it and no step down from it is allowed.
  for (int x = 2; x <= 10; ++x)
    world.blocks.emplace(x, 5, 8);
  for (double height : kHeights) {
    const NavEnvironment env = world.env(height);
    digest_cell(family.digest, env, cell(3, 1, 0)); // on the one-up step
    digest_cell(family.digest, env, cell(6, 1, 0)); // on the two-up step's lower block
    digest_cell(family.digest, env, cell(6, 2, 0)); // ... and its top
    digest_cell(family.digest, env, cell(9, 3, 0)); // the three-up shelf's top cell
    digest_cell(family.digest, env, cell(6, 6, 8)); // the four-block shelf's own cell
    digest_case(family.digest, family.tally, env, cell(1, 1, 0), cell(11, 1, 0)); // up and over
    digest_case(family.digest, family.tally, env, cell(11, 1, 0), cell(1, 1, 0)); // back down
    digest_case(family.digest, family.tally, env, cell(1, 1, 8), cell(11, 1, 8)); // along the shelf
    digest_case(family.digest, family.tally, env, cell(5, 1, 0),
                cell(5, 1, 6)); // off the shelf's side
  }
}

// --- unreachable goals -------------------------------------------------------
// A goal sealed in a shell (the partial path stops at the wall), a goal on a
// pillar top the mob cannot climb, and the same pair searched with `max_nodes`
// at 64 so the *bound* is what ends it.
void unreachable_family(Family& family) {
  World world;
  for (int x = 3; x <= 5; ++x)
    for (int y = 1; y <= 3; ++y)
      for (int z = 3; z <= 5; ++z)
        world.blocks.emplace(x, y, z); // the sealed block of rock at 4,4,4
  for (int y = 1; y <= 4; ++y)
    world.blocks.emplace(9, y, 9); // a pillar: 9,1,9 is its top cell
  for (double height : kHeights) {
    const NavEnvironment env = world.env(height);
    digest_cell(family.digest, env, cell(4, 1, 4)); // inside the sealed rock
    digest_cell(family.digest, env, cell(9, 1, 9)); // the pillar's top cell
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(4, 1, 4)); // into the rock
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(4, 1, 4), 4, 64); // bounded
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(9, 1, 9), 4,
                64); // onto the pillar
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(9, 1, 9), 4,
                16); // very bounded
  }
}

// --- buried goals ------------------------------------------------------------
// `nav_nearest_walkable`'s own shells: a goal whose exact cell is solid (the
// space above it is the answer), a goal inside a large rock (nothing standable
// within the radius, so `blocked` is true), and a goal below the floor.
void buried_family(Family& family) {
  World world;
  world.blocks.emplace(4, 1, 4); // a single block: the cell above it is walkable
  // The rock is nine wide and twelve tall, and the buried goal sits three up from
  // the floor: every cell within the shell's radius 4 is still rock (the top is
  // eight above the goal, the sides are five away), so this is the `blocked`
  // answer rather than "the space above it".
  for (int x = 8; x <= 16; ++x)
    for (int y = 1; y <= 12; ++y)
      for (int z = 8; z <= 16; ++z)
        world.blocks.emplace(x, y, z);
  for (double height : kHeights) {
    const NavEnvironment env = world.env(height);
    digest_cell(family.digest, env, cell(4, 1, 4)); // the standable cell above the block
    digest_cell(family.digest, env, cell(4, 2, 4)); // the block itself
    digest_cell(family.digest, env, cell(1, 0, 1)); // below the floor
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(4, 1, 4)); // above the block
    digest_case(family.digest, family.tally, env, cell(1, 1, 1),
                cell(12, 3, 12));                                                // inside the rock
    digest_case(family.digest, family.tally, env, cell(1, 1, 1), cell(1, 0, 1)); // below the floor
    digest_case(family.digest, family.tally, env, cell(12, 3, 12),
                cell(1, 1, 1)); // out of the rock
  }
}

bool print_mode = false;

void report(const Family& family) {
  if (print_mode) {
    std::printf("  %-12s %-34s digest %016llx  (%ld searches, %ld reached, %ld cells, %ld "
                "blocked)\n",
                family.name, family.shape, static_cast<unsigned long long>(family.digest.h),
                family.tally.searches, family.tally.reached, family.tally.cells,
                family.tally.blocked);
  }
}

} // namespace

int main(int argc, char** argv) {
  print_mode = argc > 1 && std::strcmp(argv[1], "--print") == 0;

  std::vector<Family> families = {
      {"random", "20 worlds x 3 heights x 30 pairs", {}, {}},
      {"corner", "4 orientations x 3 variants x 3 heights x 2 pairs", {}, {}},
      {"headroom", "2 ceilings x 3 heights x 4 pairs", {}, {}},
      {"ledge", "step, drops, shelf x 3 heights x 4 pairs", {}, {}},
      {"unreachable", "shell, pillar, bounds x 3 heights x 4 pairs", {}, {}},
      {"buried", "shells and rock x 3 heights x 4 pairs", {}, {}},
  };

  random_family(families[0]);
  corner_family(families[1]);
  headroom_family(families[2]);
  ledge_family(families[3]);
  unreachable_family(families[4]);
  buried_family(families[5]);

  Digest aggregate;
  Tally total;
  for (const Family& family : families) {
    aggregate.mix(family.digest.h);
    total.searches += family.tally.searches;
    total.reached += family.tally.reached;
    total.cells += family.tally.cells;
    total.blocked += family.tally.blocked;
    report(family);
  }

  // A corpus sanity check before the digest is trusted: a generator bug that
  // emptied the cases must not read as a passing gate.
  if (total.searches < 1000 || total.reached <= 0 || total.cells <= 0 || total.blocked <= 0) {
    std::fprintf(stderr,
                 "pathfinding-digest: the corpus is degenerate (searches %ld, reached %ld, cells "
                 "%ld, blocked %ld) -- fix the generator before reading the digest\n",
                 total.searches, total.reached, total.cells, total.blocked);
    return 1;
  }

  const unsigned long long got = static_cast<unsigned long long>(aggregate.h);
  std::printf("pathfinding-digest: %ld searches, %ld reached, %ld path cells, %ld blocked\n",
              total.searches, total.reached, total.cells, total.blocked);
  std::printf("pathfinding-digest: aggregate %016llx\n", got);

  if (print_mode) {
    std::printf("pathfinding-digest: --print, not comparing (re-pin kExpectedDigest to this "
                "value only for a deliberate behaviour change)\n");
    return 0;
  }
  if (aggregate.h != kExpectedDigest) {
    std::fprintf(stderr,
                 "\npathfinding-digest: FAIL -- the answers changed.\n"
                 "  expected %016llx\n"
                 "  got      %016llx\n"
                 "Some find_path / nav_nearest_walkable / nav_cell_walkable answer is not what it "
                 "was.\nIf that is intended, re-pin kExpectedDigest (run with --print) and say why "
                 "in the commit; if it is not, the change is a path-shape regression.\n",
                 static_cast<unsigned long long>(kExpectedDigest), got);
    return 1;
  }
  std::printf("pathfinding-digest: ok (answers match the pinned contract)\n");
  return 0;
}
