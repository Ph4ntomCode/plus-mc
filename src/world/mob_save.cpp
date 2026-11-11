// SPDX-License-Identifier: GPL-3.0-or-later
#include "world/mob_save.h"

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
#include "world/trade.h"

namespace mc::world {

namespace {

// The two codecs world/anvil.cpp carries for level.dat: gzip (windowBits
// 15 + 16) and the auto-detecting inflate (15 + 32, which reads zlib, gzip and
// raw). They are private to that translation unit, so the pair lives here too
// rather than one world file reaching into another's internals.
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

// Typed row reads: every field below `Id`/`Type`/`Pos` is optional, so a row
// written by an older build (or a hand-edited one) fills its defaults instead of
// failing the whole population.
std::optional<int64_t> int_field(const nbt::Value& row, const char* key) {
  const nbt::Value* v = nbt::child(row, key);
  if (v == nullptr)
    return std::nullopt;
  return nbt::as_int(*v);
}
int int_or(const nbt::Value& row, const char* key, int fallback) {
  const std::optional<int64_t> v = int_field(row, key);
  if (!v)
    return fallback;
  if (*v > 2147483647LL)
    return 2147483647;
  if (*v < -2147483648LL)
    return -2147483648;
  return static_cast<int>(*v);
}
float float_or(const nbt::Value& row, const char* key, float fallback) {
  const nbt::Value* v = nbt::child(row, key);
  if (v == nullptr)
    return fallback;
  const std::optional<double> d = nbt::as_float(*v);
  return d ? static_cast<float>(*d) : fallback;
}
bool byte_or(const nbt::Value& row, const char* key, bool fallback) {
  const nbt::Value* v = nbt::child(row, key);
  if (v == nullptr)
    return fallback;
  const std::optional<int64_t> b = nbt::as_int(*v);
  return b ? (*b != 0) : fallback;
}

} // namespace

nbt::Value mob_population_to_nbt(const std::vector<Mob>& mobs, int64_t next_entity_id) {
  using nbt::Tag;
  using nbt::Value;
  std::vector<Value> rows;
  rows.reserve(mobs.size());
  for (const Mob& mob : mobs) {
    // A removed mob is already out of the level (its death/despawn was reaped),
    // and a type-less one has nothing to name itself by in the file.
    if (mob.type == nullptr || mob.removed || !mob.is_alive())
      continue;
    std::vector<std::pair<std::string, Value>> fields;
    fields.emplace_back("Id", nbt::make_int(mob.entity_id));
    fields.emplace_back("Type", nbt::make_string(mob.type->name));
    fields.emplace_back(
        "Pos", nbt::make_list(Tag::Double, {nbt::make_double(mob.x), nbt::make_double(mob.y),
                                            nbt::make_double(mob.z)}));
    fields.emplace_back("Yaw", nbt::make_float(mob.yaw));
    fields.emplace_back("Health", nbt::make_float(mob.health));
    fields.emplace_back("Age", nbt::make_int(mob.breed_age));
    fields.emplace_back("ForcedAge", nbt::make_int(mob.forced_age));
    fields.emplace_back("InLove", nbt::make_int(mob.in_love));
    // `Animal.addAdditionalSaveData`: `EntityReference.store(loveCause, "LoveCause")`.
    // Vanilla stores a UUID reference; this engine names players, so the field
    // is the name (the same choice `mobs.dat` already makes for a killer). An
    // empty cause is written as an empty string, which is how a mod-free reload
    // tells "no cause" from a missing field.
    fields.emplace_back("LoveCause", nbt::make_string(mob.love_cause));
    fields.emplace_back("LoveTime", nbt::make_int(mob.love_time));
    fields.emplace_back("Mate", nbt::make_int(mob.mate_entity_id));
    fields.emplace_back("Parent", nbt::make_int(mob.parent_entity_id));
    fields.emplace_back("NoActionTime", nbt::make_int(mob.no_action_time));
    fields.emplace_back("PersistenceRequired", nbt::make_byte(mob.persistence_required ? 1 : 0));
    if (mob.is_villager()) {
      fields.emplace_back(
          "Villager",
          nbt::make_compound({
              {"Type", nbt::make_byte(static_cast<int8_t>(mob.villager.type))},
              {"Profession", nbt::make_byte(static_cast<int8_t>(mob.villager.profession))},
              {"Level", nbt::make_int(mob.villager.level)},
              {"Xp", nbt::make_int(mob.villager.xp)},
              {"OffersBuilt", nbt::make_byte(mob.villager.offers_built ? 1 : 0)},
          }));
    }
    rows.push_back(nbt::make_compound(std::move(fields)));
  }
  return nbt::make_compound({
      {"DataVersion", nbt::make_int(kMobPopulationDataVersion)},
      {"NextEntityId", nbt::make_long(next_entity_id)},
      {"Mobs", nbt::make_list(Tag::Compound, std::move(rows))},
  });
}

bool mob_population_from_nbt(const nbt::Value& root, MobPopulation* out) {
  if (root.type != nbt::Tag::Compound)
    return false;
  MobPopulation pop;
  if (const std::optional<int64_t> next = int_field(root, "NextEntityId"))
    pop.next_entity_id = *next;
  if (const nbt::Value* list = nbt::child(root, "Mobs")) {
    if (list->type != nbt::Tag::List || list->list_type != nbt::Tag::Compound)
      return false;
    pop.mobs.reserve(list->items.size());
    for (const nbt::Value& row : list->items) {
      if (row.type != nbt::Tag::Compound) {
        log::warn("mobs.dat: skipping a row that is not a compound");
        continue;
      }
      const std::optional<int64_t> id = int_field(row, "Id");
      const nbt::Value* name = nbt::child(row, "Type");
      const std::optional<std::string_view> type_name = name ? nbt::as_string(*name) : std::nullopt;
      const nbt::Value* pos = nbt::child(row, "Pos");
      // The three fields a row cannot be reconstructed without. A row that
      // misses one is dropped (with a note in the log) rather than guessed at.
      if (!id || !type_name || pos == nullptr || pos->type != nbt::Tag::List ||
          pos->items.size() != 3) {
        log::warn("mobs.dat: skipping a row with no id, type or position");
        continue;
      }
      std::optional<double> x = nbt::as_float(pos->items[0]);
      std::optional<double> y = nbt::as_float(pos->items[1]);
      std::optional<double> z = nbt::as_float(pos->items[2]);
      if (!x || !y || !z) {
        log::warn("mobs.dat: skipping a row whose Pos is not three numbers");
        continue;
      }
      const GeneratedMob* type = mob_by_name(*type_name);
      if (type == nullptr) {
        log::warn("mobs.dat: skipping entity ", *id, " of unknown type ", *type_name);
        continue;
      }
      Mob mob;
      mob.type = type;
      mob.entity_id = static_cast<int32_t>(*id);
      mob.x = *x;
      mob.y = *y;
      mob.z = *z;
      mob.yaw = float_or(row, "Yaw", 0.0f);
      mob.health = float_or(row, "Health", type->max_health);
      mob.breed_age = int_or(row, "Age", 0);
      mob.forced_age = int_or(row, "ForcedAge", 0);
      mob.in_love = int_or(row, "InLove", 0);
      if (const nbt::Value* cause = nbt::child(row, "LoveCause")) {
        if (const std::optional<std::string_view> name = nbt::as_string(*cause))
          mob.love_cause = std::string(*name);
      }
      mob.love_time = int_or(row, "LoveTime", 0);
      mob.mate_entity_id = int_or(row, "Mate", -1);
      mob.parent_entity_id = int_or(row, "Parent", -1);
      mob.no_action_time = int_or(row, "NoActionTime", 0);
      mob.persistence_required = byte_or(row, "PersistenceRequired", false);
      if (mob.is_villager()) {
        const nbt::Value* villager = nbt::child(row, "Villager");
        if (villager != nullptr) {
          mob.villager.type = static_cast<uint8_t>(int_or(*villager, "Type", kVillagerPlains));
          mob.villager.profession =
              static_cast<uint8_t>(int_or(*villager, "Profession", kVillagerNone));
          if (mob.villager.type >= kVillagerTypeCount)
            mob.villager.type = kVillagerPlains;
          if (mob.villager.profession >= kVillagerProfessionCount)
            mob.villager.profession = kVillagerNone;
          mob.villager.level = int_or(*villager, "Level", 1);
          if (mob.villager.level < 1 || mob.villager.level > 5)
            mob.villager.level = 1;
          mob.villager.xp = int_or(*villager, "Xp", 0);
          if (mob.villager.xp < 0)
            mob.villager.xp = 0;
          // `AbstractVillager.offers` was non-null at save time: rebuild the
          // list the same way the live level-ups built it — every level from 1
          // to the current one, each appended by `increaseMerchantCareer`'s own
          // `updateTrades`. The per-entity RNG makes the rows themselves the
          // same trades; only the live per-offer fields (uses, demand, special
          // price) are not in the file.
          if (byte_or(*villager, "OffersBuilt", false)) {
            mob.villager.offers_built = true;
            for (int level = 1; level <= mob.villager.level; ++level)
              add_villager_level_offers(&mob.villager, mob.entity_id, level);
          }
        }
      }
      if (mob.entity_id >= pop.next_entity_id)
        pop.next_entity_id = static_cast<int64_t>(mob.entity_id) + 1;
      pop.mobs.push_back(std::move(mob));
    }
  }
  *out = std::move(pop);
  return true;
}

bool write_mob_population(const std::filesystem::path& world_dir, const std::vector<Mob>& mobs,
                          int64_t next_entity_id) {
  std::error_code ec;
  if (!std::filesystem::is_directory(world_dir, ec)) {
    log::warn("mobs.dat: ", world_dir.string(), " is not a directory");
    return false;
  }
  const std::vector<uint8_t> nbt_bytes =
      nbt::serialize(mob_population_to_nbt(mobs, next_entity_id));
  std::vector<uint8_t> gz;
  if (!gzip_compress(nbt_bytes, &gz)) {
    log::error("mobs.dat: failed to compress the population");
    return false;
  }
  const std::filesystem::path path = world_dir / kMobPopulationFileName;
  const std::filesystem::path tmp = world_dir / (std::string(kMobPopulationFileName) + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      log::error("mobs.dat: cannot open ", tmp.string(), " for writing");
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
      log::error("mobs.dat: rename failed: ", rename_ec.message());
      return false;
    }
  }
  return true;
}

bool read_mob_population(const std::filesystem::path& world_dir, MobPopulation* out) {
  const std::filesystem::path path = world_dir / kMobPopulationFileName;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec))
    return true; // a fresh world: nothing to load, not a failure
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    log::warn("mobs.dat: cannot open ", path.string());
    return false;
  }
  const std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
  std::vector<uint8_t> nbt_bytes;
  if (!inflate_any(raw.data(), raw.size(), &nbt_bytes)) {
    log::warn("mobs.dat: ", path.string(), " is not gzip NBT");
    return false;
  }
  const std::optional<nbt::Value> root = nbt::parse(nbt_bytes.data(), nbt_bytes.size());
  if (!root) {
    log::warn("mobs.dat: ", path.string(), " did not parse");
    return false;
  }
  return mob_population_from_nbt(*root, out);
}

} // namespace mc::world
