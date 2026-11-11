# +MC engine (Phase 1).
# The include root is this directory, so engine code uses e.g.
#   #include "core/log.h"     #include "net/tcp_server.h"
add_library(plusmc-core STATIC
  core/metrics.cpp
  core/page_alloc.cpp
  pluginapi/plugin_host.cpp
  server/backup.cpp
  net/byte_buffer.cpp
  net/cipher_stream.cpp
  net/tcp_server.cpp
  protocol/framing.cpp
  protocol/packet_table.cpp
  server/client_config.cpp
  server/commands.cpp
  server/hosting.cpp
  server/online_auth.cpp
  server/proxy.cpp
  server/session.cpp
  server/wasm_plugins.cpp
  sim/region_scheduler.cpp
  server/status.cpp
  server/void_chunk.cpp
  translation/t0_content.cpp
  world/anvil.cpp
  world/directory_lock.cpp
  world/brewing.cpp
  world/chunk.cpp
  world/block_items.cpp
  world/block_tags.cpp
  world/consume.cpp
  world/container.cpp
  world/cooking.cpp
  world/combat.cpp
  world/crop.cpp
  world/damage.cpp
  world/effect.cpp
  world/enchantment.cpp
  world/experience_orb.cpp
  world/fire.cpp
  world/fluid.cpp
  world/chunk_generation.cpp
  world/generated_chunk.cpp
  world/leaves.cpp
  world/light.cpp
  world/world_light.cpp
  world/worldgen_biome.cpp
  world/worldgen_carver.cpp
  world/worldgen_chunk.cpp
  world/worldgen_density.cpp
  world/worldgen_dimension.cpp
  world/worldgen_feature.cpp
  world/worldgen_fossil.cpp
  world/worldgen_loot.cpp
  world/worldgen_noise.cpp
  world/worldgen_random.cpp
  world/worldgen_structure.cpp
  world/worldgen_structure_piece.cpp
  world/worldgen_surface.cpp
  world/worldgen_state.cpp
  world/mob.cpp
  world/mob_save.cpp
  world/mob_spawn.cpp
  world/random_tick.cpp
  world/redstone.cpp
  world/piston.cpp
  world/pathfinding.cpp
  world/survival.cpp
  world/use_components.cpp
  world/voxel_clip.cpp
  world/projectile.cpp
  world/weather.cpp
  world/tnt.cpp
  world/trade.cpp
  world/trim.cpp
  world/advancement.cpp
  world/advancement_save.cpp
  world/stats.cpp
  world/stats_save.cpp
  world/scoreboard.cpp
  world/scoreboard_save.cpp
  world/inventory.cpp
  world/item.cpp
  world/item_entity.cpp
  world/loot.cpp
  world/memory_world.cpp
  world/nbt.cpp
  world/potion.cpp
  world/potion_projectile.cpp
  world/recipe.cpp
  world/recipe_book.cpp
  world/registry.cpp)

target_include_directories(plusmc-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
target_link_libraries(plusmc-core PUBLIC Threads::Threads ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto
                                    ${CMAKE_DL_LIBS})
if(WIN32)
  # Winsock for the net layer; psapi for GetProcessMemoryInfo/EmptyWorkingSet in
  # core/metrics.cpp; crypt32 for the Windows certificate store OpenSSL's static
  # library reaches for when it builds a default verify path. None of the three
  # exists on the POSIX builds, so they are scoped here. crypt32 has to be
  # PUBLIC like OpenSSL itself: every executable that links plusmc-core links
  # the static OpenSSL archives through it.
  target_link_libraries(plusmc-core PUBLIC ws2_32 psapi crypt32)
endif()
# The plugin host reports this to plugins through pmc_host_api::server_version.
target_compile_definitions(plusmc-core PRIVATE PLUSMC_VERSION_STRING="${PROJECT_VERSION}")

add_executable(plusmc main.cpp)
target_link_libraries(plusmc PRIVATE plusmc-core)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-core PRIVATE -Wall -Wextra)
  target_compile_options(plusmc PRIVATE -Wall -Wextra)
endif()

# The world generator is bit-exact against a real vanilla dump, and FMA
# contraction is the one compiler freedom that can break that: Java evaluates
# `a * b + c` as two rounded operations, and an -march/-mcpu that has fused
# multiply-add would fold them into one. The mob tick's digest gate makes the
# same assumption (it was measured across both contraction settings on x86,
# where the baseline has no FMA; arm64 fuses by default). Pin contraction off
# for these translation units: noise, density, chunk fill, biome selection,
# surface, carver, feature, the two structure files (mineshaft web/density
# comparisons and structure ring trig), and the mob tick.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(
    world/worldgen_noise.cpp
    world/worldgen_density.cpp
    world/worldgen_chunk.cpp
    world/worldgen_biome.cpp
    world/worldgen_surface.cpp
    world/worldgen_carver.cpp
    world/worldgen_feature.cpp
    world/worldgen_structure.cpp
    world/worldgen_structure_piece.cpp
    world/mob.cpp
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
