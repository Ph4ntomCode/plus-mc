// SPDX-License-Identifier: GPL-3.0-or-later
//
// The advancement store's file codec (Phase 4 milestone N). See
// world/advancement_save.h for what the file is and why it exists; this file is
// the NBT schema and the gzip + atomic temp+rename wrappers, the same shape
// world/mob_save.cpp gives the mob population.
#include "world/advancement_save.h"

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

// The two codecs world/anvil.cpp carries for level.dat and world/mob_save.cpp
// carries for mobs.dat: gzip (windowBits 15 + 16) and the auto-detecting inflate
// (15 + 32, which reads zlib, gzip and raw). They are private to their
// translation units, so the pair lives here too rather than one world file
// reaching into another's internals.
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

// The same 16 MiB ceiling anvil.cpp puts on a decompressed record: a save file
// this server wrote is far smaller, and a hand-made one must not be able to
// make the boot allocate without bound.
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

// Reads `key` as a list of `type` into `*out` (nullptr when the field is
// absent). Returns false only when the field is present and is not that list:
// the container itself is then a malformed file, and a row-level field skips
// its row — the same division world/mob_save.cpp draws for `Mobs` and `Pos`.
bool list_field(const nbt::Value& node, const char* key, nbt::Tag type, const nbt::Value** out) {
  const nbt::Value* v = nbt::child(node, key);
  *out = v;
  if (v == nullptr)
    return true;
  return v->type == nbt::Tag::List && v->list_type == type;
}

} // namespace

nbt::Value advancement_state_to_nbt(const AdvancementState& state) {
  using nbt::Tag;
  using nbt::Value;
  std::vector<Value> player_rows;
  player_rows.reserve(state.players.size());
  for (const PlayerAdvancementState& player : state.players) {
    std::vector<Value> advancement_rows;
    advancement_rows.reserve(player.progress.size());
    for (const SavedAdvancementProgress& saved : player.progress) {
      std::vector<Value> criterion_rows;
      criterion_rows.reserve(saved.criteria.size());
      for (const SavedCriterion& criterion : saved.criteria) {
        criterion_rows.push_back(nbt::make_compound({
            {"Name", nbt::make_string(criterion.criterion)},
            {"Obtained", nbt::make_long(criterion.obtained_ms)},
        }));
      }
      // A row with no obtained criteria restores nothing (`hasProgress` is
      // false for it), so it is not written.
      if (criterion_rows.empty())
        continue;
      advancement_rows.push_back(nbt::make_compound({
          {"Id", nbt::make_string(saved.advancement)},
          {"Criteria", nbt::make_list(Tag::Compound, std::move(criterion_rows))},
      }));
    }
    std::vector<Value> recipe_rows;
    recipe_rows.reserve(player.known_recipes.size());
    for (const std::string& recipe : player.known_recipes)
      recipe_rows.push_back(nbt::make_string(recipe));
    // A player with nothing to remember has no row (vanilla writes them an
    // empty file, which restores the same nothing).
    if (advancement_rows.empty() && recipe_rows.empty())
      continue;
    player_rows.push_back(nbt::make_compound({
        {"Name", nbt::make_string(player.player)},
        {"Recipes", nbt::make_list(Tag::String, std::move(recipe_rows))},
        {"Advancements", nbt::make_list(Tag::Compound, std::move(advancement_rows))},
    }));
  }
  return nbt::make_compound({
      {"DataVersion", nbt::make_int(kAdvancementStateDataVersion)},
      {"Players", nbt::make_list(Tag::Compound, std::move(player_rows))},
  });
}

bool advancement_state_from_nbt(const nbt::Value& root, AdvancementState* out) {
  if (root.type != nbt::Tag::Compound)
    return false;
  AdvancementState state;
  // The container: a `Players` field that is not a list of compounds is not
  // this file's shape at all, so the whole read fails (and `out` is untouched).
  const nbt::Value* players = nullptr;
  if (!list_field(root, "Players", nbt::Tag::Compound, &players))
    return false;
  if (players != nullptr) {
    state.players.reserve(players->items.size());
    for (const nbt::Value& player_row : players->items) {
      if (player_row.type != nbt::Tag::Compound) {
        log::warn("advancements.dat: skipping a player row that is not a compound");
        continue;
      }
      // The name is the store's key: a row without one restores nothing.
      const nbt::Value* name = nbt::child(player_row, "Name");
      const std::optional<std::string_view> player_name =
          name != nullptr ? nbt::as_string(*name) : std::nullopt;
      if (!player_name || player_name->empty()) {
        log::warn("advancements.dat: skipping a player row with no name");
        continue;
      }
      PlayerAdvancementState player;
      player.player = std::string(*player_name);
      const nbt::Value* recipes = nullptr;
      if (!list_field(player_row, "Recipes", nbt::Tag::String, &recipes)) {
        log::warn("advancements.dat: skipping player ", player.player,
                  ": Recipes is not a list of names");
        continue;
      }
      if (recipes != nullptr) {
        player.known_recipes.reserve(recipes->items.size());
        for (const nbt::Value& recipe : recipes->items) {
          const std::optional<std::string_view> key = nbt::as_string(recipe);
          if (!key || key->empty()) {
            log::warn("advancements.dat: skipping a recipe row that is not a name (", player.player,
                      ")");
            continue;
          }
          player.known_recipes.push_back(std::string(*key));
        }
      }
      const nbt::Value* advancements = nullptr;
      if (!list_field(player_row, "Advancements", nbt::Tag::Compound, &advancements)) {
        log::warn("advancements.dat: skipping player ", player.player,
                  ": Advancements is not a list of compounds");
        continue;
      }
      if (advancements != nullptr) {
        player.progress.reserve(advancements->items.size());
        for (const nbt::Value& advancement_row : advancements->items) {
          if (advancement_row.type != nbt::Tag::Compound) {
            log::warn("advancements.dat: skipping an advancement row that is not a compound (",
                      player.player, ")");
            continue;
          }
          const nbt::Value* id = nbt::child(advancement_row, "Id");
          const std::optional<std::string_view> advancement_id =
              id != nullptr ? nbt::as_string(*id) : std::nullopt;
          if (!advancement_id || advancement_id->empty()) {
            log::warn("advancements.dat: skipping an advancement row with no id (", player.player,
                      ")");
            continue;
          }
          SavedAdvancementProgress saved;
          saved.advancement = std::string(*advancement_id);
          const nbt::Value* criteria = nullptr;
          if (!list_field(advancement_row, "Criteria", nbt::Tag::Compound, &criteria)) {
            log::warn("advancements.dat: skipping ", saved.advancement,
                      ": Criteria is not a list of compounds");
            continue;
          }
          if (criteria != nullptr) {
            saved.criteria.reserve(criteria->items.size());
            for (const nbt::Value& criterion_row : criteria->items) {
              if (criterion_row.type != nbt::Tag::Compound)
                continue;
              const nbt::Value* criterion_name = nbt::child(criterion_row, "Name");
              const std::optional<std::string_view> criterion =
                  criterion_name != nullptr ? nbt::as_string(*criterion_name) : std::nullopt;
              if (!criterion || criterion->empty()) {
                log::warn("advancements.dat: skipping a criterion row with no name (",
                          saved.advancement, ")");
                continue;
              }
              SavedCriterion row;
              row.criterion = std::string(*criterion);
              // `Obtained` is the instant the criterion was obtained; a row
              // without one reads as the epoch, which the wire carries as any
              // other long.
              if (const nbt::Value* obtained = nbt::child(criterion_row, "Obtained"))
                if (const std::optional<int64_t> ms = nbt::as_int(*obtained))
                  row.obtained_ms = *ms;
              saved.criteria.push_back(std::move(row));
            }
          }
          // The row's advancement may no longer exist in the generated table;
          // `apply_saved_progress` drops it there, where the lookup lives.
          if (!saved.criteria.empty())
            player.progress.push_back(std::move(saved));
        }
      }
      if (player.progress.empty() && player.known_recipes.empty()) {
        log::warn("advancements.dat: skipping player ", player.player,
                  " with no progress and no recipes");
        continue;
      }
      state.players.push_back(std::move(player));
    }
  }
  *out = std::move(state);
  return true;
}

bool write_advancement_state(const std::filesystem::path& world_dir,
                             const AdvancementState& state) {
  std::error_code ec;
  if (!std::filesystem::is_directory(world_dir, ec)) {
    log::warn("advancements.dat: ", world_dir.string(), " is not a directory");
    return false;
  }
  const std::vector<uint8_t> nbt_bytes = nbt::serialize(advancement_state_to_nbt(state));
  std::vector<uint8_t> gz;
  if (!gzip_compress(nbt_bytes, &gz)) {
    log::error("advancements.dat: failed to compress the state");
    return false;
  }
  const std::filesystem::path path = world_dir / kAdvancementStateFileName;
  const std::filesystem::path tmp = world_dir / (std::string(kAdvancementStateFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      log::error("advancements.dat: cannot open ", tmp.string(), " for writing");
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
      log::error("advancements.dat: rename failed: ", rename_ec.message());
      return false;
    }
  }
  return true;
}

bool read_advancement_state(const std::filesystem::path& world_dir, AdvancementState* out) {
  const std::filesystem::path path = world_dir / kAdvancementStateFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return true; // a fresh world: nothing to load, not a failure
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::warn("advancements.dat: cannot open ", path.string());
    return false;
  }
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
  std::vector<uint8_t> nbt_bytes;
  if (!inflate_any(raw.data(), raw.size(), &nbt_bytes)) {
    log::warn("advancements.dat: ", path.string(), " is not gzip NBT");
    return false;
  }
  const std::optional<nbt::Value> root = nbt::parse(nbt_bytes.data(), nbt_bytes.size());
  if (!root) {
    log::warn("advancements.dat: ", path.string(), " did not parse");
    return false;
  }
  return advancement_state_from_nbt(*root, out);
}

} // namespace mc::world
