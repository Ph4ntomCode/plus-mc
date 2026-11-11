// SPDX-License-Identifier: GPL-3.0-or-later
//
// The per-chunk block-tick loop: `Level.scheduleTick`'s queue and the loop that
// drains it (Phase 3 milestone G follow-up).
//
// Fire is the reason this exists. `FireBlock.tick` is not a random tick — fire is
// not a randomly-ticking block at all in 26.2 (`FireBlock` overrides neither
// `isRandomlyTicking` nor `randomTick`) — it is a *scheduled* tick: `onPlace`
// schedules the block's first tick at `30 + nextInt(10)`, and the tick then
// reschedules itself first thing, so a lit fire keeps ticking itself forever.
// Without a queue to hold those schedules every fire is inert, which is the
// limitation the fire milestone documented.
//
// The queue is bucketed by chunk so the loop only walks the chunks that have
// work, and it keeps one pending entry per position: `LevelChunkTicks.schedule`
// replaces the entry for a (position, block) pair rather than stacking a second
// one. A tick whose block is gone by the time it comes due is dropped by the
// caller, exactly as `LevelTicks` drops an entry whose block no longer matches
// (`LevelTicks.run`: `level.getBlockState(pos).is(tick.type())`).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mc::world {

struct BlockTickPos {
  int x = 0, y = 0, z = 0;

  bool operator==(const BlockTickPos&) const = default;
};

class BlockTickQueue {
public:
  // `Level.scheduleTick`: due `delay` ticks from now. A position that already
  // has a pending tick has its due time rewritten (the newest schedule wins,
  // which is `LevelChunkTicks.schedule`'s put-and-remove-from-queue).
  void schedule(int x, int y, int z, int delay) {
    const int64_t due = now_ + (delay > 0 ? delay : 1);
    std::vector<Pending>& bucket = chunks_[chunk_key(x, z)];
    for (Pending& pending : bucket) {
      if (pending.pos.x == x && pending.pos.y == y && pending.pos.z == z) {
        pending.due = due;
        return;
      }
    }
    bucket.push_back(Pending{due, BlockTickPos{x, y, z}});
  }

  // Advances the clock one tick and returns everything now due, in an
  // unspecified order (the buckets are a hash map). The caller runs one tick per
  // returned position, in that order.
  std::vector<BlockTickPos> advance() {
    ++now_;
    std::vector<BlockTickPos> due;
    for (auto it = chunks_.begin(); it != chunks_.end();) {
      std::vector<Pending>& bucket = it->second;
      size_t write = 0;
      for (size_t read = 0; read < bucket.size(); ++read) {
        if (bucket[read].due <= now_)
          due.push_back(bucket[read].pos);
        else
          bucket[write++] = bucket[read];
      }
      bucket.resize(write);
      if (bucket.empty())
        it = chunks_.erase(it);
      else
        ++it;
    }
    return due;
  }

  int64_t now() const {
    return now_;
  }

  size_t size() const {
    size_t total = 0;
    for (const auto& entry : chunks_)
      total += entry.second.size();
    return total;
  }
  bool empty() const {
    return size() == 0;
  }
  // The number of chunks holding at least one pending tick, which is what makes
  // the loop "per chunk".
  size_t chunk_count() const {
    return chunks_.size();
  }

private:
  struct Pending {
    int64_t due = 0;
    BlockTickPos pos;
  };

  // `ChunkPos.asLong`: x and z as two signed 32-bit halves.
  static int64_t chunk_key(int x, int z) {
    const int64_t cx = x >= 0 ? x / 16 : (x - 15) / 16;
    const int64_t cz = z >= 0 ? z / 16 : (z - 15) / 16;
    return (cx << 32) ^ (cz & 0xFFFFFFFFLL);
  }

  int64_t now_ = 0;
  std::unordered_map<int64_t, std::vector<Pending>> chunks_;
};

} // namespace mc::world
