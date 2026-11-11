// Level weather persistence (Phase 6): the four fields level.dat carries —
// `rainTime`, `thunderTime`, `raining`, `thundering` — survive an atomic save
// and a fresh open, and saving them merges into the file instead of replacing
// it (seed, spawn and everything else stay).
//
// The test drives the real AnvilWorld on a temporary world: open_world creates
// the minimal level.dat, the weather writer rewrites it, and a second open reads
// the fields back the way main.cpp's restore does.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>

#include "world/anvil.h"
#include "world/chunk.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                  \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

std::shared_ptr<mc::world::AnvilWorld> open(const std::filesystem::path& root) {
  return mc::world::AnvilWorld::open_world(root, mc::world::kOverworldMinSectionY,
                                           mc::world::kOverworldSectionCount);
}

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "plusmc-level-weather-test";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  auto world = open(root);
  CHECK(world != nullptr);
  if (!world)
    return 1;
  CHECK(world->ensure_level_dat());

  // A fresh minimal level.dat has no weather record: the defaults are clear.
  const auto fresh = world->level_weather();
  CHECK(fresh.has_value());
  if (fresh) {
    CHECK(!fresh->raining);
    CHECK(!fresh->thundering);
    CHECK(fresh->rain_time == 0);
    CHECK(fresh->thunder_time == 0);
    CHECK(fresh->rain_level == 0.0f);
    CHECK(fresh->thunder_level == 0.0f);
  }

  // A storm mid-ramp, saved and read back through a fresh open.
  mc::world::AnvilWorld::LevelWeather storm;
  storm.rain_time = 1200;
  storm.thunder_time = 300;
  storm.raining = true;
  storm.thundering = true;
  storm.rain_level = 0.37f;
  storm.thunder_level = 0.52f;
  CHECK(world->save_level_weather(storm));

  const auto spawn_before = world->world_spawn();
  world.reset();

  auto reopened = open(root);
  CHECK(reopened != nullptr);
  if (!reopened)
    return 1;
  const auto saved = reopened->level_weather();
  CHECK(saved.has_value());
  if (saved) {
    CHECK(saved->rain_time == 1200);
    CHECK(saved->thunder_time == 300);
    CHECK(saved->raining);
    CHECK(saved->thundering);
    CHECK(std::fabs(saved->rain_level - 0.37f) < 1e-6f);
    CHECK(std::fabs(saved->thunder_level - 0.52f) < 1e-6f);
  }

  // The writer merges: the spawn the creation path wrote is still there.
  const auto spawn_after = reopened->world_spawn();
  CHECK(spawn_before.has_value());
  CHECK(spawn_after.has_value());
  if (spawn_before && spawn_after) {
    CHECK(std::get<0>(*spawn_before) == std::get<0>(*spawn_after));
    CHECK(std::get<1>(*spawn_before) == std::get<1>(*spawn_after));
    CHECK(std::get<2>(*spawn_before) == std::get<2>(*spawn_after));
  }

  // Clearing the storm only rewrites the weather fields.
  mc::world::AnvilWorld::LevelWeather clear;
  clear.rain_time = 24000;
  clear.thunder_time = 0;
  clear.raining = false;
  clear.thundering = false;
  clear.rain_level = 0.0f;
  clear.thunder_level = 0.0f;
  CHECK(reopened->save_level_weather(clear));
  const auto after = reopened->level_weather();
  CHECK(after.has_value());
  if (after) {
    CHECK(after->rain_time == 24000);
    CHECK(after->thunder_time == 0);
    CHECK(!after->raining);
    CHECK(!after->thundering);
    CHECK(after->rain_level == 0.0f);
    CHECK(after->thunder_level == 0.0f);
  }
  reopened.reset();

  std::filesystem::remove_all(root, ec);
  if (failures == 0)
    std::printf("PASS level-weather\n");
  return failures == 0 ? 0 : 1;
}
