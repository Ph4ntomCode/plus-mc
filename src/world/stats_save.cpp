// SPDX-License-Identifier: GPL-3.0-or-later
//
// The statistics store's file codec. See world/stats_save.h for what the file
// is and why it is keyed by criterion name; this file is the NBT schema and the
// gzip + atomic temp+rename wrappers, the same shape world/advancement_save.cpp
// gives the advancement store.
#include "world/stats_save.h"

#include <zlib.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/log.h"

namespace mc::world {

namespace {

// The gzip/deflate pair the other save files carry (private per translation
// unit, so it lives here too rather than one save file reaching into another's).
bool gzip_compress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out) {
  z_stream zs{};
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  std::vector<uint8_t> buf(1 << 16);
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = deflate(&zs, Z_FINISH);
    if (rc == Z_STREAM_ERROR) {
      deflateEnd(&zs);
      return false;
    }
    const size_t produced = buf.size() - zs.avail_out;
    out->insert(out->end(), buf.data(), buf.data() + produced);
    if (rc == Z_STREAM_END)
      break;
  }
  deflateEnd(&zs);
  return rc == Z_STREAM_END;
}

// The same 16 MiB ceiling the other save readers put on a decompressed record.
constexpr size_t kMaxDecompressedBytes = 16 * 1024 * 1024;

bool inflate_any(const uint8_t* in, size_t in_len, std::vector<uint8_t>* out) {
  out->clear();
  z_stream zs{};
  if (inflateInit2(&zs, 15 + 32) != Z_OK)
    return false;
  zs.next_in = const_cast<uint8_t*>(in);
  zs.avail_in = static_cast<uInt>(in_len);
  std::vector<uint8_t> buf(1 << 16);
  int rc = Z_OK;
  while (rc == Z_OK) {
    zs.next_out = buf.data();
    zs.avail_out = static_cast<uInt>(buf.size());
    rc = inflate(&zs, Z_NO_FLUSH);
    const size_t produced = buf.size() - zs.avail_out;
    if (out->size() + produced > kMaxDecompressedBytes) {
      inflateEnd(&zs);
      return false;
    }
    out->insert(out->end(), buf.data(), buf.data() + produced);
  }
  inflateEnd(&zs);
  return rc == Z_STREAM_END;
}

// Reads `key` as a list of `type` into `*out` (nullptr when absent). Returns
// false only when the field is present and is not that list: the container is
// then malformed and the whole read fails, a row-level field skips its row.
bool list_field(const nbt::Value& node, const char* key, nbt::Tag type, const nbt::Value** out) {
  const nbt::Value* v = nbt::child(node, key);
  *out = v;
  if (v == nullptr)
    return true;
  return v->type == nbt::Tag::List && v->list_type == type;
}

} // namespace

nbt::Value stats_state_to_nbt(const StatsState& state) {
  using nbt::Tag;
  using nbt::Value;
  std::vector<Value> player_rows;
  player_rows.reserve(state.players.size());
  for (const PlayerStatsState& player : state.players) {
    std::vector<Value> stat_rows;
    stat_rows.reserve(player.stats.size());
    for (const auto& [key, value] : player.stats) {
      const std::string name = stat_criterion_name(key);
      if (name.empty())
        continue;
      stat_rows.push_back(nbt::make_compound({
          {"Name", nbt::make_string(name)},
          {"Value", nbt::make_int(value)},
      }));
    }
    // A player with no resolvable counter has no row: it would restore nothing.
    if (stat_rows.empty())
      continue;
    player_rows.push_back(nbt::make_compound({
        {"Name", nbt::make_string(player.player)},
        {"Stats", nbt::make_list(Tag::Compound, std::move(stat_rows))},
    }));
  }
  return nbt::make_compound({
      {"DataVersion", nbt::make_int(kStatsStateDataVersion)},
      {"Players", nbt::make_list(Tag::Compound, std::move(player_rows))},
  });
}

bool stats_state_from_nbt(const nbt::Value& root, StatsState* out) {
  if (root.type != nbt::Tag::Compound)
    return false;
  StatsState state;
  const nbt::Value* players = nullptr;
  if (!list_field(root, "Players", nbt::Tag::Compound, &players))
    return false;
  if (players != nullptr) {
    state.players.reserve(players->items.size());
    for (const nbt::Value& player_row : players->items) {
      if (player_row.type != nbt::Tag::Compound) {
        log::warn("stats.dat: skipping a player row that is not a compound");
        continue;
      }
      const nbt::Value* name = nbt::child(player_row, "Name");
      const std::optional<std::string_view> player_name =
          name != nullptr ? nbt::as_string(*name) : std::nullopt;
      if (!player_name || player_name->empty()) {
        log::warn("stats.dat: skipping a player row with no name");
        continue;
      }
      PlayerStatsState player;
      player.player = std::string(*player_name);
      const nbt::Value* stats = nullptr;
      if (!list_field(player_row, "Stats", nbt::Tag::Compound, &stats)) {
        log::warn("stats.dat: skipping player ", player.player,
                  ": Stats is not a list of compounds");
        continue;
      }
      if (stats != nullptr) {
        player.stats.reserve(stats->items.size());
        for (const nbt::Value& stat_row : stats->items) {
          if (stat_row.type != nbt::Tag::Compound) {
            log::warn("stats.dat: skipping a stat row that is not a compound (", player.player,
                      ")");
            continue;
          }
          const nbt::Value* stat_name = nbt::child(stat_row, "Name");
          const nbt::Value* stat_value = nbt::child(stat_row, "Value");
          const std::optional<std::string_view> criterion =
              stat_name != nullptr ? nbt::as_string(*stat_name) : std::nullopt;
          const std::optional<int64_t> value =
              stat_value != nullptr ? nbt::as_int(*stat_value) : std::nullopt;
          if (!criterion || criterion->empty() || !value) {
            log::warn("stats.dat: skipping a stat row without a name or value (", player.player,
                      ")");
            continue;
          }
          // The criterion name is the file's key: an unknown one is a stat
          // whose registry entry this build no longer carries.
          const std::optional<StatKey> key = stat_from_criterion_name(*criterion);
          if (!key) {
            log::warn("stats.dat: skipping unknown stat '", std::string(*criterion), "' (",
                      player.player, ")");
            continue;
          }
          player.stats.emplace_back(*key, static_cast<int>(*value));
        }
      }
      if (player.stats.empty()) {
        log::warn("stats.dat: skipping player ", player.player, " with no resolvable stat");
        continue;
      }
      state.players.push_back(std::move(player));
    }
  }
  *out = std::move(state);
  return true;
}

bool write_stats_state(const std::filesystem::path& world_dir, const StatsState& state) {
  std::error_code ec;
  if (!std::filesystem::is_directory(world_dir, ec)) {
    log::warn("stats.dat: ", world_dir.string(), " is not a directory");
    return false;
  }
  const std::vector<uint8_t> nbt_bytes = nbt::serialize(stats_state_to_nbt(state));
  std::vector<uint8_t> gz;
  if (!gzip_compress(nbt_bytes, &gz)) {
    log::error("stats.dat: failed to compress the state");
    return false;
  }
  const std::filesystem::path path = world_dir / kStatsStateFileName;
  const std::filesystem::path tmp = world_dir / (std::string(kStatsStateFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      log::error("stats.dat: cannot open ", tmp.string(), " for writing");
      return false;
    }
    out.write(reinterpret_cast<const char*>(gz.data()), static_cast<std::streamsize>(gz.size()));
    if (!out)
      return false;
  }
  std::error_code rename_ec;
  std::filesystem::rename(tmp, path, rename_ec);
  if (rename_ec) {
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, rename_ec);
    if (rename_ec) {
      log::error("stats.dat: rename failed: ", rename_ec.message());
      return false;
    }
  }
  return true;
}

bool read_stats_state(const std::filesystem::path& world_dir, StatsState* out) {
  const std::filesystem::path path = world_dir / kStatsStateFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return true; // a fresh world: nothing to load, not a failure
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::warn("stats.dat: cannot open ", path.string());
    return false;
  }
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
  std::vector<uint8_t> nbt_bytes;
  if (!inflate_any(raw.data(), raw.size(), &nbt_bytes)) {
    log::warn("stats.dat: ", path.string(), " is not gzip NBT");
    return false;
  }
  const std::optional<nbt::Value> root = nbt::parse(nbt_bytes.data(), nbt_bytes.size());
  if (!root) {
    log::warn("stats.dat: ", path.string(), " did not parse");
    return false;
  }
  return stats_state_from_nbt(*root, out);
}

} // namespace mc::world
