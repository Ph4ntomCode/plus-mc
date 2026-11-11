// SPDX-License-Identifier: GPL-3.0-or-later
//
// Water and lava as vanilla's block-backed fluid states. The server stores
// fluids in the existing level=N block states, but keeps the fluid rules here
// so flow distance, falling height, shapes and scheduled ticks are explicit.
#pragma once

#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "world/chunk.h"

namespace mc::world {

enum class FluidKind : uint8_t { Empty, Water, Lava };

struct FluidState {
  FluidKind kind = FluidKind::Empty;
  int amount = 0; // 0..9; source and falling water/lava are 9
  bool falling = false;
  bool operator==(const FluidState&) const = default;

  bool empty() const {
    return kind == FluidKind::Empty || amount <= 0;
  }
  bool source() const {
    return !empty() && amount == 9 && !falling;
  }
  float own_height() const {
    return static_cast<float>(amount) / 9.0f;
  }
  // FlowingFluid.getHeight: a full cell with the same fluid above is full.
  float height(const FluidState& above) const {
    return !empty() && above.kind == kind ? 1.0f : own_height();
  }
};

FluidState fluid_state_of(const BlockStateKey& state);
BlockStateKey fluid_block_state(FluidKind kind, int amount, bool falling = false);

// The cached voxel height used by Fluid.getShape. A full source with the same
// fluid above occupies a full cube; other states are a box from y=0 to height.
struct FluidShape {
  float min_y = 0.0f;
  float max_y = 0.0f;
};
FluidShape fluid_shape(const FluidState& state, const FluidState& above);

// Vanilla's horizontal loss: water drops one level, lava two in the overworld.
int fluid_drop_off(FluidKind kind);
int fluid_tick_delay(FluidKind kind);

struct FluidLevel {
  std::function<BlockStateKey(int, int, int)> get_block;
  std::function<void(int, int, int, const BlockStateKey&)> set_block;
  std::function<void(int, int, int, int)> schedule_tick;
  std::function<bool(const BlockStateKey&)> can_hold_fluid;
};

// Performs one FlowingFluid.tick. It updates this cell and schedules/spreads
// neighbors; writes are supplied by the session so block updates and lighting
// remain centralized.
void fluid_tick(FluidLevel& level, FluidKind kind, int x, int y, int z);

struct FluidTickPos {
  int x = 0, y = 0, z = 0;
  bool operator==(const FluidTickPos&) const = default;
};

class FluidTickQueue {
public:
  void schedule(int x, int y, int z, FluidKind kind, int delay);
  std::vector<std::pair<FluidKind, FluidTickPos>> advance();
  size_t size() const;
  int64_t now() const {
    return now_;
  }

private:
  struct Pending;
  // Insertion-ordered per chunk: delivery walks map order, then list order —
  // exactly the old vector's order — so same-tick fluid updates stay
  // deterministic across platforms (an unordered walk made replayed fluid
  // updates vary with the process hash seed). A list, not a vector, because
  // the dedupe index below holds iterators into it across push_back.
  using Bucket = std::list<Pending>;
  // The dedupe key is vanilla's scheduled-tick identity: the cell *and* the
  // fluid on it, so a lava entry and a water entry at one position coexist the
  // way the old per-bucket scan allowed.
  struct CellKey {
    int x, y, z;
    FluidKind kind;
    bool operator==(const CellKey&) const = default;
  };
  struct CellHash {
    size_t operator()(const CellKey& c) const {
      uint64_t h = static_cast<uint32_t>(c.x);
      h = (h ^ (static_cast<uint64_t>(static_cast<uint32_t>(c.y)) * 0x9E3779B97F4A7C15ull)) *
          0x9E3779B97F4A7C15ull;
      h = (h ^ (static_cast<uint64_t>(static_cast<uint32_t>(c.z)) * 0xC2B2AE3D27D4EB4Full)) *
          0x9E3779B97F4A7C15ull;
      h ^= static_cast<uint64_t>(c.kind) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
      return static_cast<size_t>(h ^ (h >> 32));
    }
  };
  using Index = std::unordered_map<CellKey, typename Bucket::iterator, CellHash>;
  struct Pending {
    int64_t due;
    FluidKind kind;
    FluidTickPos pos;
    // The entry's slot in the dedupe index, so `advance` unlinks it in O(1)
    // without re-hashing the key — the emission path stays as cheap as the
    // old vector drain despite the index.
    typename Index::iterator slot;
  };
  int64_t now_ = 0;
  // Ordered buckets make same-tick delivery deterministic across platforms;
  // vanilla's scheduled tick queue is priority ordered too, and an unordered
  // chunk map made replayed fluid updates vary with the process hash seed.
  std::map<int64_t, Bucket> buckets_;
  // One entry per pending tick: schedule's "vanilla retains the earliest tick"
  // is a hash lookup instead of a scan of the chunk's whole pending list — a
  // scan that grew linearly with the queue and so quadratically in the number
  // of schedules, while storms held tens of thousands of cells. Never walked:
  // delivery order comes from buckets_.
  Index queued_;
  // Nodes spliced out of the buckets as their ticks come due, held rather than
  // freed: `schedule` pops one back, so a live queue cycles the same handful
  // of nodes with no per-tick allocation — the property the old vector had by
  // keeping its capacity.
  Bucket reclaim_;
  static int64_t chunk_key(int x, int z);
};

} // namespace mc::world
