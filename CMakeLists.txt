cmake_minimum_required(VERSION 3.24)

project(plusmc
  VERSION 0.1.0
  DESCRIPTION "PlusMC — a fast, vanilla-faithful Minecraft server in C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC assumes the active code page for source files; the tree is UTF-8 (the
# comments and log strings contain non-ASCII characters), and MinGW/clang
# already read it that way. Scoped to MSVC so the other toolchains keep their
# defaults.
if(MSVC)
  add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Phase 0 tool: vanilla-data (downloads + extracts Mojang's vanilla data)
# ---------------------------------------------------------------------------
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL QUIET)

add_executable(plusmc-vanilla-data
  tools/vanilla-data/src/main.cpp)

target_include_directories(plusmc-vanilla-data PRIVATE tools/vanilla-data/src)
target_link_libraries(plusmc-vanilla-data PRIVATE ZLIB::ZLIB)

if(CURL_FOUND)
  target_link_libraries(plusmc-vanilla-data PRIVATE CURL::libcurl)
  target_compile_definitions(plusmc-vanilla-data PRIVATE HAVE_LIBCURL=1)
  message(STATUS "vanilla-data: using libcurl for downloads")
else()
  message(STATUS "vanilla-data: libcurl not found — falling back to the 'curl' CLI")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-vanilla-data PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Engine (Phase 1)
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Mod package tool (Phase 13A). This is a separate small library so a server
# can link the gate without pulling in the launcher, and the CLI can be built
# and tested on a machine with no extracted vanilla data. It shares the
# existing dependency-free JSON reader from the vanilla-data tool.
#   ./build/plusmc-mod pack --dir staging --out addon.pmcmod --key author.pem
#   ./build/plusmc-mod verify addon.pmcmod --key addon.pub.pem
# ---------------------------------------------------------------------------
add_library(plusmc-modapi STATIC
  src/modapi/mod_pack.cpp
  src/modapi/mod_package.cpp
  src/modapi/zip_archive.cpp
  src/modapi/zip_writer.cpp)
target_include_directories(plusmc-modapi
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools/vanilla-data/src)
target_link_libraries(plusmc-modapi PUBLIC OpenSSL::Crypto ZLIB::ZLIB)
target_compile_definitions(plusmc-modapi PRIVATE PLUSMC_VERSION_STRING="${PROJECT_VERSION}")
if(WIN32)
  # OpenSSL's static Windows archives reach for the system certificate store.
  target_link_libraries(plusmc-modapi PUBLIC crypt32)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-modapi PRIVATE -Wall -Wextra)
endif()

add_executable(plusmc-mod tools/plusmc_mod.cpp)
target_link_libraries(plusmc-mod PRIVATE plusmc-modapi)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-mod PRIVATE -Wall -Wextra)
endif()

# The server uses the same verifier/extractor for `--mods`.
target_link_libraries(plusmc PRIVATE plusmc-modapi plusmc-wasm-core)

# ---------------------------------------------------------------------------
# +MC Converter analyzer (Phase 7, slice 1). A static, decompile-free scan of a
# plugin JAR: the manifest and every class's constant pool, then a
# Green/Yellow/Red score. It shares the bounded ZIP reader with the package
# gate, so a hostile archive is refused by the same code path.
#   ./build/plusmc-convert analyze plugin.jar [--json]
# ---------------------------------------------------------------------------
add_library(plusmc-converter STATIC
  src/converter/class_scan.cpp
  src/converter/jar_scan.cpp
  src/converter/api_map.cpp
  src/converter/porter.cpp
  src/converter/catalog.cpp
  src/converter/publish.cpp)
target_include_directories(plusmc-converter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(plusmc-converter PUBLIC plusmc-modapi)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-converter PRIVATE -Wall -Wextra)
endif()

add_executable(plusmc-convert tools/plusmc_convert.cpp)
target_link_libraries(plusmc-convert PRIVATE plusmc-converter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-convert PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# +MC WASM plugin host (Phase 8). `inspect` is slice 1: it reads an untrusted
# `.wasm` module or a `+mc-wasm.toml` package, validates its structure with
# bounded reads, and decides whether the pinned host surface provides every
# import. `run` is slice 2: it instantiates a hostable core module in the
# engine's own interpreter and calls its lifecycle exports under the budgets.
#   ./build/plusmc-wasm inspect plugin.wasm [--json] [--require-hostable]
#   ./build/plusmc-wasm run plugin.wasm [--max-steps N] [--max-memory-pages N] ...
# ---------------------------------------------------------------------------
add_library(plusmc-wasm-core STATIC src/wasm/module.cpp src/wasm/runtime.cpp
                                  src/wasm/component_run.cpp src/world/nbt.cpp)
target_include_directories(plusmc-wasm-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(plusmc-wasm-core PUBLIC OpenSSL::Crypto)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-wasm-core PRIVATE -Wall -Wextra)
endif()

add_executable(plusmc-wasm tools/plusmc_wasm.cpp)
target_link_libraries(plusmc-wasm PRIVATE plusmc-wasm-core)
# The server's live WASM host (server/wasm_plugins.cpp) lives in plusmc-core
# and needs the runtime that the CLI also links.
target_link_libraries(plusmc PRIVATE plusmc-wasm-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-wasm PRIVATE -Wall -Wextra)
endif()

# Phase 8 slice 3: the component adapter. A separate library and executable
# while the slice-2 runtime owns the `plusmc-wasm` driver's command set, so the
# two slices land without editing one tool source.
#   ./build/plusmc-wasm-adapt plugin.wasm [--json] [--require-mapped]
add_library(plusmc-wasm-adapter STATIC src/wasm/component.cpp)
target_link_libraries(plusmc-wasm-adapter PUBLIC plusmc-wasm-core)
# The server's wasm loader maps a component's interfaces before refusing it
# (src/server/wasm_plugins.cpp), so the live binary links the adapter too.
target_link_libraries(plusmc PRIVATE plusmc-wasm-adapter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-wasm-adapter PRIVATE -Wall -Wextra)
endif()

add_executable(plusmc-wasm-adapt tools/plusmc_wasm_adapt.cpp)
target_link_libraries(plusmc-wasm-adapt PRIVATE plusmc-wasm-adapter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-wasm-adapt PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: biome map — render the worldgen biome stage over a large area (needs
# the engine, so it is defined after the core library above)
# ---------------------------------------------------------------------------
add_executable(plusmc-biome-map tools/biome_map.cpp)
target_link_libraries(plusmc-biome-map PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-biome-map PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: feature-stage golden dump — the decoration output for a small set of
# chunks at a seed, as `feature_meta.tsv` plus one gzipped cell file per chunk.
# The decoration random stream is this port's own (unlike the noise and surface
# stages), so there is no vanilla jar dump to compare against and the fixture is
# a regression pin of this runtime's own output. Regenerate deliberately:
#   ./build/plusmc-feature-dump tests/fixtures/worldgen
# ---------------------------------------------------------------------------
add_executable(plusmc-feature-dump tools/gen_feature_dump.cpp)
target_link_libraries(plusmc-feature-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-feature-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: the cell-shape probe — what a generated column's cells cost now that
# `world/section_cells.h` packs them, and how many of those bytes reach
# `core/page_alloc.h`'s cutoff (the measurement docs/PHASE5.md §4 quotes for
# "the allocator is inert for generated terrain"):
#   ./build/plusmc-cell-shape --seed 42 --columns 16
# ---------------------------------------------------------------------------
add_executable(plusmc-cell-shape tools/cell_shape_dump.cpp)
target_link_libraries(plusmc-cell-shape PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-cell-shape PRIVATE -Wall -Wextra)
endif()

# Tool: what one served (carved) generated column costs, with the quart biome
# memo's A/B arm (`PLUSMC_BIOME_QUART_MEMO=off`). The measurement behind
# docs/PHASE5.md's quart memo section.
add_executable(plusmc-bench-column tools/bench_column.cpp)
target_link_libraries(plusmc-bench-column PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-bench-column PRIVATE -Wall -Wextra)
endif()

# Tool: the chunk I/O measurements (docs/PHASE5.md, 22 Oct) — the body a chunk
# send builds, the cold read's two readers, and the disk record's bytes/sectors.
#   ./build/plusmc-bench-chunk-io wire|read|disk
add_executable(plusmc-bench-chunk-io tools/bench_chunk_io.cpp)
target_link_libraries(plusmc-bench-chunk-io PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-bench-chunk-io PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: the generation-time light seam across seeds and chunks. The measurement
# itself is `tools/light_seam.h`, shared with `world_light_test`'s pinned pair;
# this target is the many-column version and the JSON record
# `bench/seam_track.py` saves as a baseline:
#   ./build/plusmc-light-seam --seeds 42,7,2026 --radius 1 --json /tmp/seam.json
#
# `tools/check_light_seam_linux.sh` is the Linux-gcc half of that check locally:
# it builds this target in ubuntu:24.04 (native x86_64, or cross-compiled for
# aarch64 and run under qemu with `--arch arm64`) and compares the measurement
# with the pin CI uses.
# ---------------------------------------------------------------------------
add_executable(plusmc-light-seam bench/light_seam.cpp)
target_link_libraries(plusmc-light-seam PRIVATE plusmc-core)
target_include_directories(plusmc-light-seam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-light-seam PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: the decorated column as a digest, one record per build. CI runs it on
# each compiler in the build matrix and `tools/compare_column_digests.py` diffs
# the records, so a generator that only diverges on one compiler (a lifetime
# bug, an evaluation order the standard leaves unspecified) fails the push
# instead of passing each runner separately:
#   ./build/plusmc-column-digest --json /tmp/columns-gcc.json
#   python3 tools/compare_column_digests.py /tmp/columns-gcc.json /tmp/columns-clang.json
#
# `tools/check_column_digests_linux.sh` is the Linux-gcc half of that check
# locally: it builds this target in ubuntu:24.04 (native x86_64, or cross-built
# for aarch64 and run under qemu with `--arch arm64`) and compares it with the
# pin. It shares its toolchain images with `tools/check_light_seam_linux.sh`.
# ---------------------------------------------------------------------------
add_executable(plusmc-column-digest tools/column_digest.cpp)
target_link_libraries(plusmc-column-digest PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-column-digest PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: author a custom-terrain world — real Anvil region files the server
# loads through `--world`, for the path a host actually takes (someone else's
# world off disk, relit on the way out). It also writes the design manifest
# `tests/custom_world_test.py` asserts the received chunks against:
#   ./build/plusmc-gen-custom-world --out /tmp/customworld --chunks 7
#   ./build/plusmc --world /tmp/customworld --no-flat-world
# ---------------------------------------------------------------------------
add_executable(plusmc-gen-custom-world tools/gen_custom_world.cpp)
target_link_libraries(plusmc-gen-custom-world PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-gen-custom-world PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: author a *real-generator* world — the engine's own overworld generator
# (the one --worldgen serves, parity-checked against tests/fixtures/worldgen)
# written as Anvil region files at full column size, relit, for measurements
# that need a played-in column and not an authored one. The spawn square is
# centred on chunk (0,0) and `--settle-leaves` (the default) brings the leaf
# distances to the fixed point vanilla's worldgen reaches through block
# updates; `--report-leaves` prints the census that is how a treeless seed is
# chosen (a treeless spawn square was what the first residency A/B needed; the
# forested square's own A/B is in the same section of docs/PHASE5.md):
#   ./build/plusmc-gen-real-world --out /tmp/realworld --seed 48 --radius 12
#   ./build/plusmc --world /tmp/realworld --no-flat-world
# ---------------------------------------------------------------------------
add_executable(plusmc-gen-real-world tools/gen_real_world.cpp)
target_link_libraries(plusmc-gen-real-world PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-gen-real-world PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: random-tick golden dump — the random-tick pass's interaction trace for a
# small set of chunks (`random_tick_meta.tsv`, `random_tick_scene.tsv` and one
# trace per chunk). Like the feature dump above, this is a regression pin of
# this runtime's own output (the pass samples `Level.randValue`, an LCG this
# server seeds itself), so there is no vanilla jar dump to compare against.
# Regenerate deliberately:
#   ./build/plusmc-random-tick-dump tests/fixtures/random_tick
#   ./build/plusmc-random-tick-dump tests/fixtures/random_tick --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-random-tick-dump tools/gen_random_tick_dump.cpp)
target_link_libraries(plusmc-random-tick-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-random-tick-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: weather golden dump — the precipitation pass's interaction trace for a
# small set of recorded scenes (`weather_meta.tsv`, `weather_scene.tsv` and one
# trace per run). Like the two dumps above, a regression pin of this runtime's
# own output: the 1-in-48 gate comes from `Level.random` and the sampled column
# from `Level.randValue`, both the level's own streams, so there is no jar dump
# to compare against.
#   ./build/plusmc-weather-dump tests/fixtures/weather
#   ./build/plusmc-weather-dump tests/fixtures/weather --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-weather-dump tools/gen_weather_dump.cpp)
target_link_libraries(plusmc-weather-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-weather-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: block-tick golden dump — the scheduled block-tick loop's interaction
# trace for a small set of recorded scenes (`block_tick_meta.tsv`,
# `block_tick_scene.tsv`, `block_tick_actions.tsv` and one trace per run). What a
# fire schedules is a draw from the level's own `RandomSource`, so like the three
# dumps above there is no jar output to compare against and the fixture is a
# regression pin of this runtime's own output.
#   ./build/plusmc-block-tick-dump tests/fixtures/block_tick
#   ./build/plusmc-block-tick-dump tests/fixtures/block_tick --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-block-tick-dump tools/gen_block_tick_dump.cpp)
target_link_libraries(plusmc-block-tick-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-block-tick-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: fluid-tick golden dump — the scheduled fluid-tick loop's interaction
# trace for a small set of recorded scenes (`fluid_tick_meta.tsv`,
# `fluid_tick_scene.tsv`, `fluid_tick_actions.tsv` and one trace per run). The
# fluid tick takes no draw at all, so the fixture pins the whole stream rather
# than only the shape of the loop.
#   ./build/plusmc-fluid-tick-dump tests/fixtures/fluid_tick
#   ./build/plusmc-fluid-tick-dump tests/fixtures/fluid_tick --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-fluid-tick-dump tools/gen_fluid_tick_dump.cpp)
target_link_libraries(plusmc-fluid-tick-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-fluid-tick-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: region-tick golden dump — the region-parallel scheduled-tick loop's
# interaction trace for a small set of recorded scenes (`region_tick_meta.tsv`,
# `region_tick_scene.tsv`, `region_tick_actions.tsv` and one trace per run).
# This is the pin of the *partition*: which entries a tick drains, the canonical
# (region, position) order they are sorted into, the range each region is handed
# and the order that region's own entries are dispatched in. Every run is also
# executed twice — one lane and four — and the two traces must be equal for the
# runs whose work cannot cross a region boundary inside one tick; the one that
# can is recorded at a single lane and its parallel-arm difference is printed.
#   ./build/plusmc-region-tick-dump tests/fixtures/region_tick
#   ./build/plusmc-region-tick-dump tests/fixtures/region_tick --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-region-tick-dump tools/gen_region_tick_dump.cpp)
target_link_libraries(plusmc-region-tick-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-region-tick-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: mob-spawn golden dump — the natural spawner's interaction trace for a
# small set of recorded scenes (`mob_spawn_meta.tsv`, `mob_spawn_scene.tsv` and
# one trace per run). The pass is driven by `Level.random`, the level's own
# stream, so there is no jar dump to compare against.
#   ./build/plusmc-mob-spawn-dump tests/fixtures/mob_spawn
#   ./build/plusmc-mob-spawn-dump tests/fixtures/mob_spawn --diff
# ---------------------------------------------------------------------------
add_executable(plusmc-mob-spawn-dump tools/gen_mob_spawn_dump.cpp)
target_link_libraries(plusmc-mob-spawn-dump PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-mob-spawn-dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Tool: restore — the host-facing restore command (Phase 5 ops, item 8).
# Deliberately an executable of its own rather than another server flag: it has
# to answer "list my snapshots / would this restore work / restore it" while no
# server is running, and it starts nothing (no listener, no world open, no
# config). It links the engine's backup module, so it is the same code path the
# `--restore-backup` drill and `ctest -R backup` exercise:
#   ./build/plusmc-restore --world /srv/world --list
#   ./build/plusmc-restore --world /srv/world --restore 20261012T201530Z --dry-run
# ---------------------------------------------------------------------------
add_executable(plusmc-restore tools/restore.cpp)
target_link_libraries(plusmc-restore PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-restore PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# Phase 6: the native plugin API's example plugin (docs/PLUGIN_API.md,
# docs/PLAN.md §9.2). A MODULE is a shared object with no `lib` prefix, named
# as `+mc-plugin.toml`'s `library` key expects (`.so`, `.dylib`, or `.dll`); it
# links nothing from the engine, so the ABI header is genuinely the whole
# surface a plugin needs. The entry point is `PMC_PLUGIN_EXPORT`, which is
# `__declspec(dllexport)` on Windows, so the DLL has the symbol the loader
# looks up without a .def file or a whole-library export flag.
#   ./build/plusmc --plugins examples/greeter ...
# ---------------------------------------------------------------------------
add_library(plusmc-plugin-greeter MODULE examples/greeter/greeter.cpp)
set_target_properties(plusmc-plugin-greeter PROPERTIES PREFIX "" OUTPUT_NAME "greeter")
target_include_directories(plusmc-plugin-greeter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(plusmc-plugin-greeter PRIVATE -Wall -Wextra)
endif()

# The seed set (docs/PLAN.md Phase 6: five or more example plugins). Each one
# exercises a different part of ABI 1 and is small enough to read as a template.
foreach(seed caretaker announcer homes kit)
  add_library(plusmc-plugin-${seed} MODULE examples/${seed}/${seed}.cpp)
  set_target_properties(plusmc-plugin-${seed} PROPERTIES PREFIX "" OUTPUT_NAME "${seed}")
  target_include_directories(plusmc-plugin-${seed} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-plugin-${seed} PRIVATE -Wall -Wextra)
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Fuzz targets (opt-in; need the protocol core built above)
# ---------------------------------------------------------------------------
option(PLUSMC_BUILD_FUZZERS "Build protocol fuzz targets" OFF)
if(PLUSMC_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
  # The pre-velocity ABI fixture for `plugin_test.py` (see
  # tests/fixtures/old_player_plugin.cpp): a module that declares the original
  # `pmc_player` and `pmc_entity` prefixes, calls `player_get` and `entity_get`
  # with those shorter sizes, rejects structs below the player/entity floor,
  # checks a larger caller learns the host's fill size, pins the visitor's
  # struct_size, and rejects a short pmc_item. Testing-only, unlike the example
  # module.
  add_library(plusmc-plugin-oldabi MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_player_plugin.cpp)
  set_target_properties(plusmc-plugin-oldabi PROPERTIES PREFIX "" OUTPUT_NAME "oldabi")
  target_include_directories(plusmc-plugin-oldabi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-plugin-oldabi PRIVATE -Wall -Wextra)
  endif()
  # The original-host-table fixture: a module that reads only the prefix of
  # `pmc_host_api` (through `server_version`) and checks the host advertises at
  # least that much. Testing-only, unlike the example module.
  add_library(plusmc-plugin-oldhost MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_host_plugin.cpp)
  set_target_properties(plusmc-plugin-oldhost PROPERTIES PREFIX "" OUTPUT_NAME "oldhost")
  target_include_directories(plusmc-plugin-oldhost PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-plugin-oldhost PRIVATE -Wall -Wextra)
  endif()
  # The refuse-after-registering fixture: it registers a tick listener and a
  # command, then returns 1 from on_load. The host must purge both before it
  # closes the module, or the next tick calls into unmapped code. Testing-only.
  add_library(plusmc-plugin-refusing MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/refusing_plugin.cpp)
  set_target_properties(plusmc-plugin-refusing PROPERTIES PREFIX "" OUTPUT_NAME "refusing")
  target_include_directories(plusmc-plugin-refusing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-plugin-refusing PRIVATE -Wall -Wextra)
  endif()
  # Milestone A: NBT parser against the committed flat-world chunk fixture.
  add_executable(plusmc-nbt-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/nbt_fixture_test.cpp)
  target_link_libraries(plusmc-nbt-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-nbt-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME nbt-fixture
    COMMAND plusmc-nbt-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/r.0.0.mca)
  # Milestone A: the chunk encoder reproduces the captured vanilla body for
  # the same chunk byte-for-byte (fixture region dir + reference hex).
  add_executable(plusmc-encode-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/encode_fixture_test.cpp)
  target_link_libraries(plusmc-encode-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-encode-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME encode-fixture
    COMMAND plusmc-encode-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/chunk000_0_0_body.hex)
  # Milestone-A.5: a real overworld chunk (5- and 6-bit paletted containers,
  # fluids, multi-biome sections) also reproduces its captured vanilla body
  # byte-for-byte; see tests/encode_ow3_test.cpp.
  add_executable(plusmc-encode-ow3-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/encode_ow3_test.cpp)
  target_link_libraries(plusmc-encode-ow3-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-encode-ow3-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME encode-ow3
    COMMAND plusmc-encode-ow3-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ow3_chunk_n2m1_body.hex)
  # Milestone B: global/direct palette mode — 257 distinct block states in
  # one section, encoded at 15 bits with no palette list.
  add_executable(plusmc-encode-global-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/encode_global_test.cpp)
  target_link_libraries(plusmc-encode-global-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-encode-global-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME encode-global
    COMMAND plusmc-encode-global-test)
  # Milestone F: block-drop resolution (tool gate, property rules, chance
  # pools) against the generated loot table.
  add_executable(plusmc-loot-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/loot_test.cpp)
  target_link_libraries(plusmc-loot-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-loot-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME loot
    COMMAND plusmc-loot-test)
  # Milestone F: the drop path's Add Entity / Set Entity Data bytes, pinned
  # against the vanilla 26.2 codecs (no "present" byte before the item id).
  add_executable(plusmc-inventory-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/inventory_test.cpp)
  target_link_libraries(plusmc-inventory-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-inventory-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME inventory
    COMMAND plusmc-inventory-test)
  # Milestone C: the container block-entity NBT codec and the chunk round-trip
  # that carries it to disk and back.
  add_executable(plusmc-container-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_test.cpp)
  target_link_libraries(plusmc-container-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-container-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME container
    COMMAND plusmc-container-test)

  # Milestone D: the crafting recipe engine over the generated table.
  add_executable(plusmc-recipe-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_test.cpp)
  target_link_libraries(plusmc-recipe-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-recipe-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME recipe
    COMMAND plusmc-recipe-test)

  # Milestone D2: armor trims — the generated trim registries, the
  # smithing_trim matcher and the minecraft:trim component on the wire.
  add_executable(plusmc-trim-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/trim_test.cpp)
  target_link_libraries(plusmc-trim-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-trim-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME trim
    COMMAND plusmc-trim-test)

  # Milestone D2: the recipe book — the display list over the whole table, the
  # update_recipes / recipe_book_add / settings / ghost bodies, and place_recipe.
  add_executable(plusmc-recipe-book-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_book_test.cpp)
  target_link_libraries(plusmc-recipe-book-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-recipe-book-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME recipe-book
    COMMAND plusmc-recipe-book-test)

  # Milestone E: the furnace family — the fuel table, cooking recipe lookup,
  # the smelting tick, the block entity's timers and the experience owed.
  add_executable(plusmc-cooking-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/cooking_test.cpp)
  target_link_libraries(plusmc-cooking-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-cooking-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME cooking
    COMMAND plusmc-cooking-test)

  # Milestone E follow-up: experience orbs as entities — the value ladder, the
  # launch, gravity/drag/landing, the pull, merging, the lifetime and pickup.
  add_executable(plusmc-xp-orb-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/xp_orb_test.cpp)
  target_link_libraries(plusmc-xp-orb-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-xp-orb-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME xp-orb
    COMMAND plusmc-xp-orb-test)

  # Phase 4 follow-up: dropped items as real server entities — the launch, the
  # box and the generous pickup area (Player.aiStep's inflate(1.0, 0.5, 1.0)),
  # the merge rule, gravity/drag, the move gate, the bounce, the 6000-tick
  # lifetime and the pickup report (world/item_entity.{h,cpp}).
  add_executable(plusmc-item-entity-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_entity_test.cpp)
  target_link_libraries(plusmc-item-entity-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-item-entity-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME item_entity
    COMMAND plusmc-item-entity-test)

  # Milestone I: the ground pathfinder — walkability, detours, step up/down,
  # partial paths and the bounded search.
  add_executable(plusmc-pathfinding-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/pathfinding_test.cpp)
  target_link_libraries(plusmc-pathfinding-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-pathfinding-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME pathfinding
    COMMAND plusmc-pathfinding-test)

  # The pathfinder's answer contract, pinned as a digest of every
  # nav_nearest_walkable / nav_cell_walkable / find_path answer over a
  # deterministic corpus (see the test's own header). A change that moves a path
  # fails the test until the constant is deliberately re-pinned.
  add_executable(plusmc-pathfinding-digest-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/pathfinding_digest_test.cpp)
  target_link_libraries(plusmc-pathfinding-digest-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-pathfinding-digest-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME pathfinding-digest
    COMMAND plusmc-pathfinding-digest-test)

  # Milestone I: mobs — the stat table, the physics, the AI (target scan,
  # chase, melee), the external hurt path, the loot table and the packets.
  add_executable(plusmc-mob-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_test.cpp)
  target_link_libraries(plusmc-mob-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-mob-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME mob
    COMMAND plusmc-mob-test)

  # The mob tick's answer contract, pinned as a digest of every `tick_mobs`
  # answer, RNG draw and mob field over a deterministic five-scene corpus (see
  # the test's own header). A change that moves a position, a path, a report
  # vector or a draw fails the test until the constant is deliberately
  # re-pinned.
  add_executable(plusmc-mob-digest-test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_digest_test.cpp)
  target_link_libraries(plusmc-mob-digest-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-mob-digest-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME mob-digest
    COMMAND plusmc-mob-digest-test)

  # The digests above are pinned with `MobGrid` on. The grid-vs-scan A/B
  # (docs/PHASE5.md, "Where the switch sits") needs the other arm as a *build*,
  # not an environment variable, so that no shipped binary carries a switch for
  # the grid: `world/mob.cpp` alone differs between the arms, so it is compiled
  # a second time with PLUSMC_MOB_GRID_OFF and that object is linked into a
  # second copy of the digest test. The archive's own `mob.o` is never pulled,
  # because every symbol it defines is already defined by this object -- the two
  # arms would fail to link, not disagree quietly. `mob-grid-ab` runs this
  # binary and that one and compares them; the scan binary is not a test of its
  # own, since on its own it would pin nothing new.
  #
  # EXCLUDE_FROM_ALL: an ordinary build compiles `mob.cpp` once, as it did
  # before the A/B existed. Ask for the arm when the equivalence needs
  # re-checking -- `cmake --build <dir> --target plusmc-mob-digest-test-scan`,
  # then `ctest -R mob-grid-ab`. Until it is built the test skips with 77:
  # a build input that is not there, not a pass. CI builds this target before
  # running ctest and fails the job if the test still skips, so the gate compares
  # the arms on every push.
  add_library(plusmc-mob-scan-arm OBJECT EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/src/world/mob.cpp)
  target_compile_definitions(plusmc-mob-scan-arm PRIVATE PLUSMC_MOB_GRID_OFF=1)
  # Usage requirements (the `src/` include root, the thread/zlib/OpenSSL
  # headers) are all this object needs; nothing is linked from plusmc-core here.
  target_link_libraries(plusmc-mob-scan-arm PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-mob-scan-arm PRIVATE -Wall -Wextra)
    # The archive's own `mob.cpp` has contraction pinned off (src/CMakeLists.txt);
    # the A/B only means anything if this second compilation uses the same
    # arithmetic. Without it arm64 fuses one arm and not the other, and the
    # grid/scan scenes disagree for a reason that has nothing to do with MobGrid.
    target_compile_options(plusmc-mob-scan-arm PRIVATE -ffp-contract=off)
  endif()
  add_executable(plusmc-mob-digest-test-scan EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_digest_test.cpp
    $<TARGET_OBJECTS:plusmc-mob-scan-arm>)
  target_link_libraries(plusmc-mob-digest-test-scan PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-mob-digest-test-scan PRIVATE -Wall -Wextra)
  endif()

  # The serverbound packet audit: the interaction ranges every positional
  # handler validates against (the block sphere, the box-vs-centre measurement
  # and the cursor tolerance).
  add_executable(plusmc-effect-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/effect_test.cpp)
  target_link_libraries(plusmc-effect-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-effect-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME effect
    COMMAND plusmc-effect-test)

  # The milestone G follow-up: `minecraft:consumable`'s on_consume_effects list
  # — the generated table, the probability gate, applying/removing/clearing and
  # LivingEntity.randomTeleport's landing search.
  add_executable(plusmc-consume-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/consume_test.cpp)
  target_link_libraries(plusmc-consume-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-consume-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME consume
    COMMAND plusmc-consume-test)

  add_executable(plusmc-use-components-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/use_components_test.cpp)
  target_link_libraries(plusmc-use-components-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-use-components-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME use-components
    COMMAND plusmc-use-components-test)

  # The milestone G follow-up: drinking a potion — the generated
  # minecraft:potion table, the minecraft:potion_contents component (both
  # wire directions) and PotionContents.applyToLivingEntity's instant/lasting
  # split, plus /give's item[potion_contents={...}] argument form.
  add_executable(plusmc-potion-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/potion_test.cpp)
  target_link_libraries(plusmc-potion-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-potion-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME potion
    COMMAND plusmc-potion-test)

  # The milestone G follow-up: thrown potions — `ThrowablePotionItem.use`'s
  # launch, `ThrowableProjectile.tick`'s gravity/drag/clip, the three impact
  # branches (a water bottle, a splash potion's 4 x 2 x 4 falloff, a lingering
  # potion's cloud) and `AreaEffectCloud.serverTick`.
  add_executable(plusmc-potion-projectile-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/potion_projectile_test.cpp)
  target_link_libraries(plusmc-potion-projectile-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-potion-projectile-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME potion-projectile
    COMMAND plusmc-potion-projectile-test)

  # The milestone G follow-up: fire — the generated flammability table, the
  # placement/survival rules, the water bottle's douse, the burning countdown
  # and `BaseFireBlock.entityInside`'s box walk.
  add_executable(plusmc-fire-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fire_test.cpp)
  target_link_libraries(plusmc-fire-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-fire-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fire
    COMMAND plusmc-fire-test)

  # Phase 4 milestone D: fluid state heights, vanilla water/lava drop-off,
  # downward and lateral spreading, waterlogging and the fluid tick queue.
  add_executable(plusmc-fluid-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fluid_test.cpp)
  target_link_libraries(plusmc-fluid-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-fluid-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fluid
    COMMAND plusmc-fluid-test)

  # Phase 4 milestone D follow-up: the scheduled fluid-tick loop — the queue's
  # clock and per-tick drain, the dispatch by the entry's own kind, the falling
  # and lateral ladders, the retraction cascade and the caller's own placements
  # and breaks. The rules' property halves live in tests/fluid_test.cpp; this
  # binary owns the loop. The golden half replays the recorded scenes and compares
  # every interaction trace row for row (regenerate with
  # tools/gen_fluid_tick_dump.cpp via the plusmc-fluid-tick-dump target).
  add_executable(plusmc-fluid-tick-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fluid_tick_test.cpp)
  target_link_libraries(plusmc-fluid-tick-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-fluid-tick-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME fluid_tick
    COMMAND plusmc-fluid-tick-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fluid_tick)

  # Phase 4 milestone E seed: deterministic redstone attenuation and bounded
  # connected-component recomputation.
  add_executable(plusmc-redstone-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/redstone_test.cpp)
  target_link_libraries(plusmc-redstone-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-redstone-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME redstone
    COMMAND plusmc-redstone-test)

  # Phase 4 milestone F: piston movement, sticky pulls, and the twelve-block
  # push limit over pure world callbacks.
  add_executable(plusmc-piston-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/piston_test.cpp)
  target_link_libraries(plusmc-piston-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-piston-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME piston
    COMMAND plusmc-piston-test)

  # Phase 4 milestone G core: attack charge/critical/sweep/knockback formulas
  # and shield damage gating.
  add_executable(plusmc-combat-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/combat_test.cpp)
  target_link_libraries(plusmc-combat-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-combat-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME combat
    COMMAND plusmc-combat-test)

  # Phase 4 milestone H core: the bow's charge curve, the arrow's launch, its
  # tick order (move, then drag, then gravity), the bury and the 1200-tick
  # despawn, the damage/Power/critical/pierce/Punch rules, and the three thrown
  # items (world/projectile.{h,cpp}).
  add_executable(plusmc-projectile-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/projectile_test.cpp)
  target_link_libraries(plusmc-projectile-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-projectile-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME projectile
    COMMAND plusmc-projectile-test)

  # TNT: the generated blast-resistance table, the explosion ray-cast, the
  # visibility sample, the damage and knockback, and the primed entity's own
  # fall and fuse (world/tnt.{h,cpp}).
  add_executable(plusmc-tnt-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/tnt_test.cpp)
  target_link_libraries(plusmc-tnt-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS)
    target_compile_options(plusmc-tnt-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME tnt
    COMMAND plusmc-tnt-test)

  # Phase 4 milestone L: villager trading — the generated trade table, the
  # offer arithmetic (modified cost, uses, demand, special prices), the
  # without-duplicates pick and the villager's xp/level bookkeeping
  # (world/trade.{h,cpp}).
  add_executable(plusmc-trade-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/trade_test.cpp)
  target_link_libraries(plusmc-trade-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-trade-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME trade
    COMMAND plusmc-trade-test)

  # Phase 4 milestone M: statistics and the scoreboard — the two registry
  # orders (the nine stat types, the 77 custom stats), the criterion names,
  # and the scoreboard's objectives/scores/display slots/teams with their
  # change log (world/stats.{h,cpp}, world/scoreboard.{h,cpp}).
  add_executable(plusmc-stats-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/stats_test.cpp)
  target_link_libraries(plusmc-stats-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-stats-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME stats
    COMMAND plusmc-stats-test)

  add_executable(plusmc-scoreboard-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoreboard_test.cpp)
  target_link_libraries(plusmc-scoreboard-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-scoreboard-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME scoreboard
    COMMAND plusmc-scoreboard-test)

  # ...and the milestone's packets: the award-stats, objective, score, reset,
  # display-slot and team builders pinned byte-for-byte against the 26.2
  # codecs (server/scoreboard_packets.h).
  add_executable(plusmc-scoreboard-packets-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoreboard_packets_test.cpp)
  target_link_libraries(plusmc-scoreboard-packets-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-scoreboard-packets-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME scoreboard-packets
    COMMAND plusmc-scoreboard-packets-test)

  # Milestone N: the advancement registry, the progress/requirements model, the
  # trigger events, the visibility walk and the flush (world/advancement.h).
  add_executable(plusmc-advancement-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/advancement_test.cpp)
  target_link_libraries(plusmc-advancement-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-advancement-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME advancement
    COMMAND plusmc-advancement-test)

  # ...and the milestone's packets: update_advancements and
  # select_advancements_tab pinned byte-for-byte against the 26.2 codecs
  # (server/advancement_packets.h).
  add_executable(plusmc-advancement-packets-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/advancement_packets_test.cpp)
  target_link_libraries(plusmc-advancement-packets-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-advancement-packets-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME advancement-packets
    COMMAND plusmc-advancement-packets-test)

  # ...and the store's save codec: the progress rows and known recipes
  # round-tripping through the file's NBT, the reload a fresh model adopts, and
  # the rows a reader must skip (world/advancement_save.{h,cpp}).
  add_executable(plusmc-advancement-save-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/advancement_save_test.cpp)
  target_link_libraries(plusmc-advancement-save-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-advancement-save-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME advancement-save
    COMMAND plusmc-advancement-save-test)

  # ...and the two stores beside it: a player's stat counters (keyed by
  # `Stat.buildName`'s criterion name, so a registry renumbering cannot move a
  # count) and the server-wide scoreboard (objectives, read-only stat scores,
  # display slots and teams) round-tripping through their files
  # (world/stats_save.{h,cpp}, world/scoreboard_save.{h,cpp}).
  add_executable(plusmc-stats-save-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/stats_save_test.cpp)
  target_link_libraries(plusmc-stats-save-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-stats-save-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME stats-save
    COMMAND plusmc-stats-save-test)

  add_executable(plusmc-scoreboard-save-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoreboard_save_test.cpp)
  target_link_libraries(plusmc-scoreboard-save-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-scoreboard-save-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME scoreboard-save
    COMMAND plusmc-scoreboard-save-test)

  # Milestone J: the random-tick pass — the generated randomly-ticking table,
  # the crop's growth speed and light gate, the leaf's distance and decay, the
  # per-section tick count and the pass's own sampling (world/random_tick.h,
  # world/crop.h, world/leaves.h). The golden half replays the recorded scene and
  # compares the whole interaction trace row for row, which is what pins the
  # sampled positions the shape assertions cannot (regenerate with
  # tools/gen_random_tick_dump.cpp via the plusmc-random-tick-dump target).
  add_executable(plusmc-random-tick-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/random_tick_test.cpp)
  target_link_libraries(plusmc-random-tick-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-random-tick-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME random_tick
    COMMAND plusmc-random-tick-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/random_tick)

  # Phase 3 milestone G follow-up: the scheduled block-tick loop — the queue's
  # clock and the per-tick drain, what the dispatch does with each due position
  # (a fire ticks as fire, a leaf as a leaf, anything else is dropped), and the
  # caller's own placements and breaks. The queue's own property half lives in
  # tests/fire_test.cpp and the leaf rules' in tests/random_tick_test.cpp; this
  # binary owns the loop. The golden half replays the recorded scenes and compares
  # every interaction trace row for row (regenerate with
  # tools/gen_block_tick_dump.cpp via the plusmc-block-tick-dump target).
  add_executable(plusmc-block-tick-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_tick_test.cpp)
  target_link_libraries(plusmc-block-tick-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-block-tick-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME block_tick
    COMMAND plusmc-block-tick-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/block_tick)

  # Phase 4 milestone C: natural spawning — the generated biome spawn lists and
  # their renormalization, `SpawnState`'s caps and charges, the light/placement/
  # distance gates, the pass over a dark and a lit platform, and
  # `Mob.checkDespawn`. The golden half replays the recorded scenes and compares
  # the pass's whole interaction trace row for row, which is what pins the draw
  # chain a scripted-draw test cannot (regenerate with tools/gen_mob_spawn_dump.cpp
  # via the plusmc-mob-spawn-dump target).
  add_executable(plusmc-mob-spawn-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_spawn_test.cpp)
  target_link_libraries(plusmc-mob-spawn-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-mob-spawn-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME mob_spawn
    COMMAND plusmc-mob-spawn-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mob_spawn)

  # Phase 4: the mob population's save codec — entity ids and the animal state
  # that references them (`Mate`, `Parent`, the love clocks) round-tripping
  # through the file's NBT, the id counter never falling behind its own rows,
  # a villager's rebuilt offers, and the rows a reader must skip
  # (world/mob_save.{h,cpp}).
  add_executable(plusmc-mob-save-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_save_test.cpp)
  target_link_libraries(plusmc-mob-save-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-mob-save-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME mob_save
    COMMAND plusmc-mob-save-test)

  # Milestone H: weather — the generated biome climate table, the rain/thunder
  # cycle and its levels, `Level.precipitationAt`, the cauldron's
  # `handlePrecipitation` and `tickChunk`'s one-in-48 sample. The golden half
  # replays the recorded scenes and compares the whole interaction trace row for
  # row, which is what pins the sequence a scripted-draw test cannot (regenerate
  # with tools/gen_weather_dump.cpp via the plusmc-weather-dump target).
  add_executable(plusmc-weather-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/weather_test.cpp)
  target_link_libraries(plusmc-weather-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-weather-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME weather
    COMMAND plusmc-weather-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/weather)

  # The milestone G follow-up: brewing a potion in world — the generated
  # brewing table (PotionBrewing.addVanillaMixes), its lookups and `mix`, the
  # brewing stand's 400-tick serverTick, the `has_bottle_0..2` bits, the
  # ingredient's crafting remainder and the block entity's NBT round-trip.
  add_executable(plusmc-brewing-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/brewing_test.cpp)
  target_link_libraries(plusmc-brewing-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-brewing-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME brewing
    COMMAND plusmc-brewing-test)

  add_executable(plusmc-enchantment-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/enchant_test.cpp)
  target_link_libraries(plusmc-enchantment-test PRIVATE plusmc-core)
  if(WARNINGS_AS_ERRORS OR PLUSMC_WERROR)
    target_compile_options(plusmc-enchantment-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME enchant
    COMMAND plusmc-enchantment-test)

  add_executable(plusmc-interaction-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/interaction_test.cpp)
  target_link_libraries(plusmc-interaction-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-interaction-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME interaction
    COMMAND plusmc-interaction-test)

  # Phase 4 milestone A: the light engine — the occluding/emitting tables, the
  # nibble packing, skylight and its column search, block light's propagation,
  # and the strongest check available: whether it reproduces the light a real
  # vanilla chunk carries on disk.
  add_executable(plusmc-light-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/light_test.cpp)
  target_link_libraries(plusmc-light-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-light-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME light
    COMMAND plusmc-light-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

  # Phase 4 milestone K: the overworld world generator against a real vanilla
  # dump — the seed stack, all 63 noise parameter sets, `base_3d_noise`, the
  # router's splines, `getInterpolatedNoiseValue`, and two whole noise-stage
  # chunks byte-for-byte (16 x 16 x 384 cells each). The fixtures come from
  # tools/gen_worldgen_dump.py, which boots the server jar's own registries.
  add_executable(plusmc-worldgen-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_test.cpp)
  target_link_libraries(plusmc-worldgen-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-worldgen-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME worldgen
    COMMAND plusmc-worldgen-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worldgen)

  add_executable(plusmc-worldgen-dimension-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_dimension_test.cpp)
  target_link_libraries(plusmc-worldgen-dimension-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-worldgen-dimension-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME worldgen-dimension
    COMMAND plusmc-worldgen-dimension-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worldgen)

  # Phase 4 milestone K's feature stage: the carried worldgen registries
  # (configured/placed features, carvers, structures, structure sets and the
  # full living-entity registry) and the decoration pass that consumes the
  # feature registry. The registry half is compiled in; the golden half reads
  # `feature_meta.tsv` + `feature_seed42_c*.bin.gz` from the fixtures dir — the
  # port's own decoration output, pinned so the random stream cannot drift
  # silently (regenerate with tools/gen_feature_dump.cpp via the
  # plusmc-feature-dump target).
  add_executable(plusmc-worldgen-feature-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_feature_test.cpp)
  target_link_libraries(plusmc-worldgen-feature-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-worldgen-feature-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME worldgen-feature
    COMMAND plusmc-worldgen-feature-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/worldgen)

  # Phase 4's structure pieces: the hand-coded swamp hut, igloo, buried
  # treasure, desert pyramid, jungle temple, mineshaft, stronghold, ocean
  # monument and nether fortress, the template shipwreck, the ocean-ruin/
  # ruined-portal/village starts, and the pillager-outpost, ancient-city,
  # trail-ruins, bastion and end-city assemblies. Each writer is pinned against
  # the 26.2 server's own `postProcess` over synthetic terrain, and the starts/
  # apply path against real candidate chunks. Strongholds add the ring list and
  # an all-63-chunk bulk dump; monuments add all 16 chunks of their 58 x 58
  # box, and mansions add all 30 chunks of a 514-piece start plus three other
  # rotations, so the test takes tens of seconds rather than a few.
  add_executable(plusmc-worldgen-structure-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_structure_test.cpp)
  target_link_libraries(plusmc-worldgen-structure-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-worldgen-structure-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME worldgen-structure
    COMMAND plusmc-worldgen-structure-test)

  # Phase 4 milestone K's structure loot: every generated chest/dispenser
  # table, the deterministic resolver, and the chest/spawner block entities the
  # seed-42 shipwreck and monster rooms actually carry. Eleven columns and no
  # ring lists, so it runs in about a second.
  add_executable(plusmc-worldgen-loot-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_loot_test.cpp)
  target_link_libraries(plusmc-worldgen-loot-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-worldgen-loot-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME worldgen-loot
    COMMAND plusmc-worldgen-loot-test)

  # Phase 4 milestone B: the engine's bridge to a living world — the light
  # section index a packet uses, which writes mark which chunks, what a relight
  # of a real overworld column costs and reports, and the clock-and-weather sky
  # darken the random-tick pass's light gate reads.
  add_executable(plusmc-world-light-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/world_light_test.cpp)
  target_link_libraries(plusmc-world-light-test PRIVATE plusmc-core)
  # `light_seam.h`: the seam comparison shared with `plusmc-light-seam`.
  target_include_directories(plusmc-world-light-test PRIVATE
                             ${CMAKE_CURRENT_SOURCE_DIR}/tools)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-world-light-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME world_light
    COMMAND plusmc-world-light-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

  # The relight against the world it borrows from: the store's own exclusion is
  # held for the whole borrow-and-write (the fix for the session-thread SIGSEGVs
  # in convo/request_All_chunk_stream_light_crash.json), pinned deterministically
  # and under the concurrent shape — several session-shaped threads relighting
  # the same chunks while others copy and read them.
  add_executable(plusmc-light-relight-race-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/light_relight_race_test.cpp)
  target_link_libraries(plusmc-light-relight-race-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-light-relight-race-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME light_relight_race
    COMMAND plusmc-light-relight-race-test)

  # Phase 5 ordered item 4, the eviction half: the world store's chunk budget.
  # A population's view set is bounded by it (that is why the 40-bot spread-256
  # soak plateaus at ~2 GB), and the eviction it runs when it is full must take
  # a column it can regenerate before one it cannot, because a written column is
  # the world's only copy of that write (there is no region file under
  # MemoryWorld). Server-free: it drives the store directly.
  add_executable(plusmc-chunk-cache-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/chunk_cache_test.cpp)
  target_link_libraries(plusmc-chunk-cache-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-chunk-cache-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME chunk_cache
    COMMAND plusmc-chunk-cache-test)

  # Ahead-of-view generation (world/chunk_generation.h): the pool a join storm
  # hands its view to. Server-free and world-cheap (a void store whose builds
  # are air plus a sleep), it pins the four properties the stream path relies
  # on — a request covers the 3x3 ring, a duplicate ask is not a second build,
  # a full queue drops instead of blocking, and the destructor joins rather than
  # exiting under a worker.
  add_executable(plusmc-chunk-generation-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/chunk_generation_test.cpp)
  target_link_libraries(plusmc-chunk-generation-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-chunk-generation-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME chunk_generation
    COMMAND plusmc-chunk-generation-test)

  # Phase 5 ordered item 3: the region scheduler's own contract — the chunk ->
  # region mapping, the deterministic per-region streams, and the phase's three
  # promises (every region exactly once on lane `i % lanes`, the lane count
  # invisible in a region's result, a lane's failure surfacing after every lane
  # is joined). Server-free on purpose: it pins the scheduler the tick path
  # borrows without starting a world, a listener or a bot.
  add_executable(plusmc-region-scheduler-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/region_scheduler_test.cpp)
  target_link_libraries(plusmc-region-scheduler-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-region-scheduler-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME region-scheduler
    COMMAND plusmc-region-scheduler-test)

  # Phase 5 ordered item 3 follow-up: the slicer the entity passes put on top of
  # the scheduler. Every entity exactly once, a coupled pair never split by a
  # region edge, the slice list in the scheduler's canonical order, and the
  # restore an identity for a tick that removed nothing and exactly the erased
  # entities for one that did. Server-free: it moves stand-in entities only.
  add_executable(plusmc-region-entities-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/region_entities_test.cpp)
  target_link_libraries(plusmc-region-entities-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-region-entities-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME region-entities
    COMMAND plusmc-region-entities-test)

  # Phase 5 ordered item 3 follow-up: the same scheduler, but on the loop it
  # actually runs — the scheduled-tick drain's region snapshot. The meta names
  # each run's lane count (the parallel arm for every lane-stable run, so the
  # replay is also the lane-count check) and the test re-derives every tick's
  # ranges from its own `due` rows. The rules' own streams stay
  # `tests/fixtures/block_tick`'s; this one holds the partition.
  add_executable(plusmc-region-tick-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/region_tick_test.cpp)
  target_link_libraries(plusmc-region-tick-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-region-tick-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME region_tick
    COMMAND plusmc-region-tick-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/region_tick)

  # The world stores' height reads, on both backends: a write is visible to the
  # very next height question through the const interface (the read that used to
  # bind a stale overload and answer a file-backed world's pre-write height), a
  # live `modify_chunk` reference, the raw arrays of the copy `load_chunk` hands
  # the wire encoder, and a column read back from disk after a flush.
  add_executable(plusmc-heightmap-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/heightmap_test.cpp)
  target_link_libraries(plusmc-heightmap-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-heightmap-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME heightmap
    COMMAND plusmc-heightmap-test
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

  # Phase 6: the level's weather survives a restart. `level.dat` carries the
  # four weather fields, the writer merges them into the existing file (seed and
  # spawn survive), and a fresh open reads them back.
  add_executable(plusmc-level-weather-test
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/level_weather_test.cpp)
  target_link_libraries(plusmc-level-weather-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-level-weather-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME level_weather
    COMMAND plusmc-level-weather-test)

  # Milestone F: the survival layer — damage types, armour, the damage
  # pipeline, hunger, air, fall damage, eating and the packets they raise.
  add_executable(plusmc-survival-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/survival_test.cpp)
  target_link_libraries(plusmc-survival-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-survival-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME survival
    COMMAND plusmc-survival-test)

  add_executable(plusmc-item-table-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_table_test.cpp)
  target_include_directories(plusmc-item-table-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  if(NOT MSVC)
    target_compile_options(plusmc-item-table-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME item-table
    COMMAND plusmc-item-table-test)

  add_executable(plusmc-component-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/component_test.cpp)
  target_link_libraries(plusmc-component-test PRIVATE plusmc-core)
  if(NOT MSVC)
    target_compile_options(plusmc-component-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME component
    COMMAND plusmc-component-test)

  add_executable(plusmc-drop-packet-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/drop_packet_test.cpp)
  target_link_libraries(plusmc-drop-packet-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-drop-packet-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME drop-packet
    COMMAND plusmc-drop-packet-test)

  # Phase-5 follow-up (the write path, docs/WRITE_PATH.md): the framing split
  # that lets one compressed chunk frame be shared by many connections — the
  # encoder's bytes must equal write_frame's exactly, and a verbatim write must
  # reproduce them on a second connection.
  add_executable(plusmc-framing-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/framing_test.cpp)
  target_link_libraries(plusmc-framing-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-framing-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME framing
    COMMAND plusmc-framing-test)

  # Phase-1 follow-up: the generated packet-id tables — table shape, every
  # clientbound constant resolved back to its canonical name and id, and the
  # hand-written serverbound dispatch table cross-checked id for id against
  # vanilla's own 776 registration order.
  add_executable(plusmc-protocol-ids-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_ids_test.cpp)
  target_link_libraries(plusmc-protocol-ids-test PRIVATE plusmc-core)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(plusmc-protocol-ids-test PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME protocol-ids
    COMMAND plusmc-protocol-ids-test)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    # The wire client's transcript layer (tests/wire_client.py): record a run's
    # packets to a file, replay them without a server. No binary, no fixtures.
    add_test(NAME transcript
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/transcript_test.py)
    add_test(NAME vanilla-data-smoke
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/vanilla_data_smoke.py
              $<TARGET_FILE:plusmc-vanilla-data>)
    # The one bug class that is invisible on a single platform: two calls that
    # advance one shared generator inside a single expression, where which draw
    # lands in which parameter is the compiler's choice. clang evaluates
    # arguments left to right and gcc right to left, so the same seed generated
    # stone on macOS and water on Linux (docs/LINUX_PORT.md, "The worldgen
    # divergence"). The scan is static, needs no binary and no fixtures, and
    # runs in well under a second; `src/` is the hard-fail scope.
    add_test(NAME rng-order
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_rng_order.py
              --repo ${CMAKE_CURRENT_SOURCE_DIR}
              --explain)
    set_tests_properties(rng-order PROPERTIES LABELS "static")
    # The cross-compiler decorated-column comparison's own test. The comparator
    # is what fails CI when one runner's columns move, and — with --baseline —
    # when every runner moves together; if it stopped noticing either difference
    # the guard would be silently green while every runner passed its own tests.
    # The test feeds it identical, moved, missing, extra, duplicate and
    # differently shaped records, plus the agreement-without-the-pin case, and
    # needs no binary and no fixtures.
    add_test(NAME column-digest-compare
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/column_digest_test.py)
    set_tests_properties(column-digest-compare PROPERTIES LABELS "static")
    # The debug preview's block textures come from Java client assets, not from
    # the server. This gate resolves every block state through the source's
    # blockstates/models and compares the preview's sampled texels with the
    # source PNGs. The default source is the newest prepared tree (or a client
    # jar); the 26.2 variant scores that tree's own block-state subset. Mojang's
    # data is gitignored, so a machine without it skips (77).
    add_test(NAME block-texture-parity
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_texture_parity_test.py
              --repo "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(block-texture-parity PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    add_test(NAME block-texture-parity-26.2
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_texture_parity_test.py
              --repo "${CMAKE_CURRENT_SOURCE_DIR}"
              --assets "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.2/assets")
    set_tests_properties(block-texture-parity-26.2 PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    # Real model boxes: a bottom slab and bottom stair must let a ray through
    # their empty half instead of behaving like a full cube.
    add_test(NAME block-geometry
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_geometry_test.py)
    set_tests_properties(block-geometry PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    # Item-model coverage, flat item texel parity, block item icons and the
    # inventory sheet. Each source is scored over its own item definitions, so
    # the 26.2 variant does not count 26.3-only items as missing. Mojang's data
    # is gitignored, so a machine without it skips rather than scoring itself.
    add_test(NAME item-texture-parity
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_texture_parity_test.py)
    set_tests_properties(item-texture-parity PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    add_test(NAME item-texture-parity-26.2
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_texture_parity_test.py
              --assets "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.2/assets")
    set_tests_properties(item-texture-parity-26.2 PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    # The local-only prepare step: extract the client jar's asset tree and the
    # matching game asset store (sounds included) into data/vanilla/<version>/
    # assets. The test uses a synthetic jar/index and needs no Mojang files.
    add_test(NAME vanilla-assets
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/vanilla_assets_test.py)
    set_tests_properties(vanilla-assets PROPERTIES LABELS "static")
    # The read side of the same tree: block models and PNGs resolve from a
    # prepared assets directory without reopening the client jar.
    add_test(NAME texture-source
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/texture_source_test.py)
    set_tests_properties(texture-source PROPERTIES LABELS "static")
    # The preview's HUD and inventory sheet follow the server's player-menu
    # content/update packets, not a local sample dictionary.
    add_test(NAME live-inventory
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_inventory_test.py)
    set_tests_properties(live-inventory PROPERTIES LABELS "static")
    # Every item component id in the 26.3 registry must consume exactly its
    # own bytes in a clientbound stack.
    add_test(NAME item-component-codecs
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_component_codecs_test.py)
    set_tests_properties(item-component-codecs PROPERTIES LABELS "static")
    # Scene-level comparison needs Java reference screenshots captured at the
    # same viewport; the test always self-checks the comparator, then skips 77
    # when no reference directory exists.
    add_test(NAME scene-texture-parity
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_texture_parity_test.py)
    set_tests_properties(scene-texture-parity PROPERTIES
                         LABELS "static" SKIP_RETURN_CODE 77)
    # The light-seam comparison's own test. `bench/seam_track.py` is the pin CI
    # runs (`--fail-on-change` against SEAM_BASELINE) and the comparison
    # `tools/check_light_seam_linux.sh` uses for its container legs, which hand
    # it a measurement file instead of a running probe. The test drives that
    # form: a matching measurement passes, a moved tally fails only under the
    # flag (naming the field and the delta), a different set of columns is a
    # shape change, and a missing file, junk, another record kind or a file
    # with no tallies is refused rather than compared as an empty measurement.
    add_test(NAME light-seam-track
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/light_seam_track_test.py)
    set_tests_properties(light-seam-track PROPERTIES LABELS "static")
    # The `pen` scene's grid-vs-scan A/B (docs/PHASE5.md, "Where the switch
    # sits"): the digest corpus ticked on both arms -- the shipped binary, and
    # the one built above with `MobGrid` compiled out. Every per-scene digest
    # and the aggregate must match, and the script also fails when the binaries
    # do not report different arms -- a define that stopped being honoured must
    # not pass as an equivalence. Two runs of a one-second binary, so unlike
    # `load-stability` this belongs in the gate -- when the scan build is
    # present, which EXCLUDE_FROM_ALL makes an explicit ask. Without it the
    # script exits 77 and prints the target to build, so a tree that has not
    # built the arm reads as skipped instead of as a green comparison.
    add_test(NAME mob-grid-ab
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_mob_grid_ab.py
              $<TARGET_FILE:plusmc-mob-digest-test>
              $<TARGET_FILE:plusmc-mob-digest-test-scan>)
    set_tests_properties(mob-grid-ab PROPERTIES SKIP_RETURN_CODE 77)
    # Negative control for the positive place-back frame assertion: an
    # otherwise identical restored frame with one pixel changed must fail.
    add_test(NAME player-session-frame-check
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/player_session_frame_check_test.py)
    set_tests_properties(player-session-frame-check PROPERTIES LABELS "static")
    if(TARGET plusmc)
      # End-to-end player session: real login/chunk/block packets, break and
      # place-back, the rendered frames and the live inventory sheet. The test
      # checks every artifact and enforces a fixed wall-clock budget, so this
      # stays a smoke rather than a screenshot benchmark.
      add_test(NAME player-session-smoke
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/player_session_smoke_test.py
                $<TARGET_FILE:plusmc>)
      set_tests_properties(player-session-smoke PROPERTIES
                           LABELS "smoke" RUN_SERIAL TRUE
                           SKIP_RETURN_CODE 77 TIMEOUT 600)
      # Negative companion: run a real session with place-back intentionally
      # skipped. Its server.log must not satisfy the exact placement assertion;
      # this keeps the positive smoke from passing if that check breaks.
      add_test(NAME player-session-smoke-negative
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/player_session_smoke_test.py
                --negative-place-back $<TARGET_FILE:plusmc>)
      set_tests_properties(player-session-smoke-negative PROPERTIES
                           LABELS "smoke" RUN_SERIAL TRUE
                           SKIP_RETURN_CODE 77 TIMEOUT 300)
      # The same preview smoke against each prepared local asset tree. These
      # are opt-in by data: the trees are git-ignored and only registered when
      # `tools/vanilla_assets.py` has prepared them. Each test sets
      # PLUSMC_ASSETS so it cannot silently fall back to the other release's
      # tree (or a cached jar).
      foreach(version IN ITEMS 26.2 26.3)
        set(preview_assets "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/${version}/assets")
        if(EXISTS "${preview_assets}/minecraft/blockstates"
           AND EXISTS "${preview_assets}/minecraft/models")
          add_test(NAME preview-smoke-${version}
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/player_session_smoke_test.py
                    $<TARGET_FILE:plusmc>)
          set_tests_properties(preview-smoke-${version} PROPERTIES
                               LABELS "smoke;preview" RUN_SERIAL TRUE
                               ENVIRONMENT "PLUSMC_ASSETS=${preview_assets}"
                               SKIP_RETURN_CODE 77 TIMEOUT 600)
        endif()
      endforeach()
      add_test(NAME status-ping
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/status_ping_test.py
                $<TARGET_FILE:plusmc>)
      # Phase 9 T1: status negotiation for 766-774, and the explicit refusal
      # until the T1 login/configuration/play translators land.
      add_test(NAME status-t1
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/status_t1_test.py
                $<TARGET_FILE:plusmc>)
      # Phase 9 T1 slice 2: 1.21.11 login + configuration. Needs the captured
      # 1.21.11 clientconfig (not synthesised from 26.3 data).
      if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/1.21.11/clientconfig/tags.bin")
        add_test(NAME t1-config
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/t1_config_test.py
                  $<TARGET_FILE:plusmc>)
      endif()
      # T0 26.1/26.2: their own login/config assets and 775/776 play entry.
      if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.1/clientconfig/tags.bin"
         AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.1.1/clientconfig/tags.bin"
         AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.1.2/clientconfig/tags.bin"
         AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/26.2/clientconfig/tags.bin")
        add_test(NAME t0-config
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/t0_config_test.py
                  $<TARGET_FILE:plusmc>)
      endif()
      # Opt-in real vanilla-client check for T0. It skips (77) unless a
      # 26.1/26.2 jar is supplied explicitly; a supplied jar that cannot reach
      # play is a failure, not a skip.
      add_test(NAME real-client-smoke
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/real_client_smoke_test.py
                $<TARGET_FILE:plusmc>)
      set_tests_properties(real-client-smoke PROPERTIES
                           LABELS "smoke;real-client" RUN_SERIAL TRUE
                           SKIP_RETURN_CODE 77 TIMEOUT 600)
      # The loader half of the plugin API, without a joining client: this is
      # the plugin test that runs on a tree with no extracted vanilla data,
      # and it is the one that exercises the platform loader (LoadLibrary on
      # Windows, dlopen elsewhere) plus the ABI refusal and unload path. The
      # old-host fixture also loads here, so the host table's prefix rule is
      # covered on a runner that cannot join a player; the refusing fixture
      # proves a plugin that registers and then refuses on_load is purged
      # before its module is closed.
      if(TARGET plusmc-plugin-greeter AND TARGET plusmc-plugin-oldhost
         AND TARGET plusmc-plugin-refusing)
        add_test(NAME plugin-loader
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_loader_test.py
                  $<TARGET_FILE:plusmc>
                  $<TARGET_FILE:plusmc-plugin-greeter>
                  ${CMAKE_CURRENT_SOURCE_DIR}/examples/greeter/+mc-plugin.toml
                  $<TARGET_FILE:plusmc-plugin-oldhost>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_host_plugin.toml
                  $<TARGET_FILE:plusmc-plugin-refusing>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/refusing_plugin.toml)
      endif()
      # Phase 6 seed set: the four examples load and enable together, register
      # their commands, and unload in the ordinary shutdown path. No client, so
      # this runs on a runner without vanilla data.
      if(TARGET plusmc-plugin-caretaker AND TARGET plusmc-plugin-announcer
         AND TARGET plusmc-plugin-homes AND TARGET plusmc-plugin-kit)
        add_test(NAME plugin-seeds
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_seeds_test.py
                  $<TARGET_FILE:plusmc>
                  $<TARGET_FILE:plusmc-plugin-caretaker>
                  ${CMAKE_CURRENT_SOURCE_DIR}/examples/caretaker/+mc-plugin.toml
                  $<TARGET_FILE:plusmc-plugin-announcer>
                  ${CMAKE_CURRENT_SOURCE_DIR}/examples/announcer/+mc-plugin.toml
                  $<TARGET_FILE:plusmc-plugin-homes>
                  ${CMAKE_CURRENT_SOURCE_DIR}/examples/homes/+mc-plugin.toml
                  $<TARGET_FILE:plusmc-plugin-kit>
                  ${CMAKE_CURRENT_SOURCE_DIR}/examples/kit/+mc-plugin.toml)
        set_tests_properties(plugin-seeds PROPERTIES LABELS "static")
      endif()
      # Phase 13A: signed .pmcmod preflight, deterministic pack, verified
      # extract and the real server --mods path. Builds archives and signs them
      # with the CLI, so it needs no vanilla data and no joining client.
      if(TARGET plusmc-mod AND TARGET plusmc-plugin-greeter)
        add_test(NAME mod-package
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mod_package_test.py
                  $<TARGET_FILE:plusmc-mod>
                  $<TARGET_FILE:plusmc-plugin-greeter>
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(mod-package PROPERTIES LABELS "static")
      endif()
      # Phase 7 slice 1: the converter analyzer. Builds synthetic class files
      # and JARs in the test, so it needs no Java toolchain and no fixtures.
      if(TARGET plusmc-convert)
        add_test(NAME converter-analyze
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_test.py
                  $<TARGET_FILE:plusmc-convert>)
        set_tests_properties(converter-analyze PROPERTIES LABELS "static")
      endif()
      # Phase 7 slice 2: the reference mapper. Same generated fixtures, one
      # report per status (mapped / partial / gap) and the JSON shape.
      if(TARGET plusmc-convert)
        add_test(NAME converter-map
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_map_test.py
                  $<TARGET_FILE:plusmc-convert>)
        set_tests_properties(converter-map PROPERTIES LABELS "static")
      endif()
      # Phase 7 slice 3: the porter. The test compiles the generated scaffold
      # against the real ABI with the engine's own compiler, so a scaffold that
      # stops building fails here rather than at a user's first port.
      if(TARGET plusmc-convert)
        add_test(NAME converter-port
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_port_test.py
                  $<TARGET_FILE:plusmc-convert>
                  ${CMAKE_CXX_COMPILER}
                  ${CMAKE_CURRENT_SOURCE_DIR}
                  ${CMAKE_COMMAND})
        set_tests_properties(converter-port PROPERTIES LABELS "static")
      endif()
      # Phase 7 slice 4: the catalog index. Same generated fixtures; the test
      # pins the status of every kind of jar and the index file.
      if(TARGET plusmc-convert)
        add_test(NAME converter-catalog
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_catalog_test.py
                  $<TARGET_FILE:plusmc-convert>)
        set_tests_properties(converter-catalog PROPERTIES LABELS "static")
      endif()
      # Phase 7 slice 9: deployment, run-over-run history and the review
      # signature. Uses the module packer's `keygen` so the site and the mod
      # format are signed by the same Ed25519 key format.
      if(TARGET plusmc-convert AND TARGET plusmc-mod)
        add_test(NAME converter-publish
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/converter_publish_test.py
                  $<TARGET_FILE:plusmc-convert>
                  $<TARGET_FILE:plusmc-mod>)
        set_tests_properties(converter-publish PROPERTIES LABELS "static")
      endif()
      # Phase 8 slice 1: the WASM package gate. Builds real WebAssembly bytes
      # in Python (no toolchain, no checked-in binaries) and pins the hostable
      # and blocked verdicts plus every structural refusal.
      if(TARGET plusmc-wasm)
        add_test(NAME wasm-module
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_module_test.py
                  $<TARGET_FILE:plusmc-wasm>)
        set_tests_properties(wasm-module PROPERTIES LABELS "static")
      endif()
      # Phase 8 slice 2: the runtime. The same in-Python assembler builds
      # modules that are actually executed, so the test pins the interpreter's
      # arithmetic, control flow, memory and host calls, every budget that
      # stops a module, and the traps an invalid body raises.
      if(TARGET plusmc-wasm)
        add_test(NAME wasm-runtime
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_runtime_test.py
                  $<TARGET_FILE:plusmc-wasm>)
        set_tests_properties(wasm-runtime PROPERTIES LABELS "static")
      endif()
      # Phase 8 slice 3: the component adapter. Reads the component layer's
      # imports/exports and maps them onto the pinned pumpkin:plugin@0.2.0
      # table; it executes nothing.
      if(TARGET plusmc-wasm-adapt)
        add_test(NAME wasm-component
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_component_test.py
                  $<TARGET_FILE:plusmc-wasm>
                  $<TARGET_FILE:plusmc-wasm-adapt>)
        set_tests_properties(wasm-component PROPERTIES LABELS "static")
      endif()
      # ...and the interpreter's numeric and stateful core, which the lifecycle
      # walk touches only lightly: `br_table`, `select`, an `if` with a result,
      # the i64/f32/f64 operations, mutable globals, the start section, the
      # config round trip and the per-instruction step check.
      if(TARGET plusmc-wasm)
        add_test(NAME wasm-core
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_core_test.py
                  $<TARGET_FILE:plusmc-wasm>)
        set_tests_properties(wasm-core PROPERTIES LABELS "static")
      endif()
      # Phase 8 slice 3, execution half: a spec-valid component whose
      # lifecycle exports are canon lifts of zero-argument core functions runs
      # end to end; imports, canon lower, canonical options and every other
      # shape are refused by name before anything executes.
      if(TARGET plusmc-wasm)
        add_test(NAME wasm-component-run
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_component_run_test.py
                  $<TARGET_FILE:plusmc-wasm>)
        set_tests_properties(wasm-component-run PROPERTIES LABELS "static")
      endif()
      # Phase 8 slice 4: the live-session host. Starts a real server with a
      # `+mc-wasm.toml` package, checks the module's load/enable log through the
      # server host and its config file, then checks the disable on shutdown.
      if(TARGET plusmc)
        add_test(NAME wasm-server
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_server_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(wasm-server PROPERTIES LABELS "static")
      endif()
      # Login Plugin Request/Response needs no client-config assets.
      add_test(NAME login-plugin
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/login_plugin_test.py
                $<TARGET_FILE:plusmc>)
      # Phase-5 ordered item 7: proxy forwarding at join. Legacy BungeeCord
      # (the NUL-separated handshake address) and Velocity modern (the signed
      # velocity:player_info exchange) both hand the session a UUID/profile it
      # did not derive from Login Start; the test pins that substitution, that
      # it is opt-in, and that a forged Velocity signature is refused. Both are
      # also run through a live proxy (tests/proxy_live.py), which writes the
      # handshake the backend sees and answers the Velocity request itself.
      # Stops at Login Success, so it needs no client-config assets either.
      add_test(NAME proxy-forward
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/proxy_forward_test.py
                $<TARGET_FILE:plusmc>)
      # Phase 5 (ops): the two halves of item 8 that are not backups
      # (docs/PHASE5.md §6 item 8). Neither needs client-config assets or a
      # joining client: `--validate-config` starts nothing at all, and
      # `--metrics-file` runs an idle server for a few seconds and reads the
      # JSON-lines export it writes. Deliberately outside the client-config
      # branch below, so they run on a tree with no extracted vanilla data.
      add_test(NAME config-validate
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_validate_test.py
                $<TARGET_FILE:plusmc>)
      add_test(NAME metrics-file
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics_file_test.py
                $<TARGET_FILE:plusmc>)
      # A long soak stopped with Ctrl-C still writes the window it measured
      # (bench/soak.py's interrupt path): the record is marked interrupted, the
      # server log is written, and the harness exits 130. Labelled `soak` like
      # the other cells that start real clients.
      add_test(NAME soak-interrupt
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/soak_interrupt_test.py
                $<TARGET_FILE:plusmc>)
      # The test now runs a short normal join preflight before the interrupted
      # window; allow for one busy-box retry without truncating the evidence.
      set_tests_properties(soak-interrupt PROPERTIES TIMEOUT 240 LABELS "soak")
      # The full-join test needs extracted vanilla client-config assets
      # (generated by tools/vanilla-data + a vanilla capture; not committed).
      file(GLOB _client_config_tags
        "${CMAKE_CURRENT_SOURCE_DIR}/data/vanilla/*/clientconfig/tags.bin")
      if(_client_config_tags)
        add_test(NAME join
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/join_test.py
                  $<TARGET_FILE:plusmc>)
        # The forwarded address at the ops surface: a full join through the live
        # proxy, with the address the proxy names distinct from its own socket
        # peer, asserting the join and disconnect lines carry it. (The login
        # line already did; this is the session behind it.)
        add_test(NAME proxy-join
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/proxy_join_test.py
                  $<TARGET_FILE:plusmc>)
        # The light pool's post-leave return, asserted rather than described
        # (docs/PHASE5.md §4): two arms of one small cell differing only in
        # PLUSMC_LIGHT_ALLOC, gated on the pool's own booked pages after the same
        # 192 columns are released. Fails when the store never filled or the
        # pooled arm logged no release pass, rather than skipping: a loaded box
        # cannot make the cell, and that is a run to redo on a quiet one, not a
        # missing input to hide behind a skip code. Joins clients, so it lives
        # here with the other join tests.
        add_test(NAME light-pool-leave
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/light_pool_leave_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(light-pool-leave PROPERTIES
          TIMEOUT 900 LABELS "soak")
        add_test(NAME join-online
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_join_test.py
                  $<TARGET_FILE:plusmc>)
        # The other half of the same lookup: the https session server must be
        # verified. A self-signed impostor that answers hasJoined 200 for any
        # name is refused before its handler runs.
        add_test(NAME join-online-tls
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/online_auth_tls_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase-2 milestone A: real chunks stream from an Anvil world; the
        # streamed chunk (0,0) is byte-identical to the vanilla capture.
        add_test(NAME world-join
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/world_join_test.py
                  $<TARGET_FILE:plusmc>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
        # Phase-4 milestone K integration: --worldgen streams the built-in
        # generator's overworld; the streamed chunks (0,0) and (-1,-1) are
        # cell-for-cell the vanilla noise-stage dumps, with their heightmaps,
        # biomes and light.
        add_test(NAME worldgen-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/worldgen_wire_test.py
                  $<TARGET_FILE:plusmc>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
        # Vanilla parity, measured rather than claimed: the default (decorated)
        # arm of `--worldgen` is streamed to a client and compared cell for cell
        # with the game's own post-carver dumps. Biomes must still be exact; the
        # block divergence is counted and pinned (tests/vanilla_parity_test.py),
        # and docs/PARITY_REPORT.md collects the stage rows and the gaps.
        add_test(NAME vanilla-parity
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/vanilla_parity_test.py
                  $<TARGET_FILE:plusmc>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
        # A world this server did not make: `--world` pointed at a directory of
        # real Anvil region files authored by tools/gen_custom_world.cpp (a
        # stepped pyramid with a house on it, a lake with a beach, a mesa, a cave
        # tunnel, a shaft into a lava pool, an iron vein, trees), served to a real
        # client and decoded with the independent decoder. Every cell named by the
        # authoring tool's manifest is compared against the bytes the client
        # received, chunks outside the authored region must be the void fallback,
        # the apex heightmap must agree with the blocks under it, and the sky
        # light on the wire must come from the relight (the file stores none).
        # It ends with the server's own RAM/CPU report for the run.
        add_test(NAME custom-world
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/custom_world_test.py
                  $<TARGET_FILE:plusmc>
                  $<TARGET_FILE:plusmc-gen-custom-world>)
        # Phase 4's Tier-C ledger: measures how many of the 26.2 pack's biome,
        # worldgen-feature/carver/structure, enchantment, advancement and mob
        # registries this engine actually carries (docs/PLAN.md §3.1's "Tier C
        # >= 90% measured"). It reads the pack and the generated tables, so it
        # needs no server and opens no socket; `--check` asserts the ledger is
        # well-formed, and the run prints the item- and category-weighted
        # numbers into the log. It deliberately does not assert the 90%
        # threshold — that is the phase gate, and the ledger is what says how
        # far away it is.
        add_test(NAME tier-coverage
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tools/tier_c_coverage.py
                  --check --repo ${CMAKE_CURRENT_SOURCE_DIR})
        # Phase 4 milestone C: `NaturalSpawner` over the wire — a flat plains
        # chunk spawns nothing in daylight, monsters at midnight, and nothing at
        # all inside the player's own 24 blocks.
        add_test(NAME spawn-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/spawn_wire_test.py
                  $<TARGET_FILE:plusmc>
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)
        # Join/leave churn + concurrent movement (registry broadcast safety;
        # the interesting run is under ASan).
        add_test(NAME join-churn
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/churn_test.py
                  $<TARGET_FILE:plusmc> 4)
        # Milestone C: block place/break with creative-mode inventory.
        add_test(NAME block-interact
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_interact_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 3 milestone C: block containers — open a chest, move items in
        # with container_click, and reopen it after a server restart.
        add_test(NAME container-gui
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/container_gui_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 3 milestone C: default chest state, double-chest pairing, and
        # splitting a pair on break.
        add_test(NAME double-chest
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/double_chest_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 3 milestone D: crafting — the 3x3 table, the player's 2x2, the
        # derived result slot, consumption and the shift-click repeat loop.
        add_test(NAME craft
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/craft_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 3 milestone D2: the recipe book — the join push
        # (update_recipes / recipe_book_settings / recipe_book_add) decodes the
        # way the client reads it, and place_recipe fills the grid from a display
        # id out of that push.
        add_test(NAME recipe-book-join
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/recipe_book_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone D: the stonecutter menu — opening it, picking a recipe over
        # container_button_click, taking the result and the selection reset.
        add_test(NAME stonecutter
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/stonecutter_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone E: the furnace cooks over the real wire protocol — lit
        # block state, flame/progress data slots, output and the experience.
        add_test(NAME furnace
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/furnace_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone F: survival over the wire — the join health push, armour
        # and its attribute modifiers, /damage and /kill, the death sequence,
        # respawning with the dropped inventory, and hunger/eating.
        add_test(NAME health
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/health_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone F follow-up, and the thing a survival world is
        # unplayable without: a broken block's drop is a *real entity* over the
        # wire — it falls and rests on the floor, two drops merge into one
        # stack (the survivor's Set Entity Data goes to 2 and the absorbed one
        # is despawned), nothing is collected from three blocks away, walking
        # into the drop sends take_item_entity and puts the stack in the
        # player's own menu, and a creative `set_creative_slot` at slot -1
        # throws a real entity from the eye height without writing a slot.
        add_test(NAME item-pickup-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_pickup_test.py
                  $<TARGET_FILE:plusmc>)
        # `RangedBowAttackGoal` over the wire: a pig at the same distance never
        # shoots, a summoned skeleton looses an arrow from its own eye, the
        # arrow's position syncs advance, and the hit lands on the player's
        # packet clock as a `set_health` drop.
        add_test(NAME skeleton-arrow-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/skeleton_arrow_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone G: status effects and enchantment protection over the wire —
        # update_mob_effect/remove_mob_effect and their flags, the effect's own
        # attribute modifiers, /effect and /enchant, Resistance and Protection
        # turning a real hit aside, and the effect set ending with the player.
        add_test(NAME effects
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/effects_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone G follow-up: minecraft:consumable's on_consume_effects
        # over the wire — a golden apple granting Absorption and Regeneration
        # (with the absorption points eating a real hit), a milk bucket clearing
        # everything, a honey bottle removing poison alone, a chorus fruit hop
        # and a silent ominous bottle.
        add_test(NAME consume-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/consume_test.py
                  $<TARGET_FILE:plusmc>)

        # Fluid resumption: a block removed beside settled fluid schedules the
        # fluid ticks that can now flow into the opening (the shared
        # block-removal neighbour fan-out), and nothing flows while the wall
        # stands. Complements consume-wire above: that one runs the fluid tick,
        # this one proves a break can start it.
        add_test(NAME fluid-resume-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fluid_resume_test.py
                  $<TARGET_FILE:plusmc>)
        # The live redstone graph: a redstone block beside a wire run settles it
        # through the command's own neighbour pass, the run reads the vanilla
        # 15, 14, 13 ... ladder one wire per step, a lamp beside the first wire
        # lights while one past the fifteenth step stays dark, and cutting the
        # source relaxes the run back. The bug it pins fed a wire's own power
        # back in as its neighbour's source, so every run read 15.
        add_test(NAME redstone-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/redstone_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # The drain direction of the same fan-out: cutting off a stream's
        # source retracts the downstream cells, in cascade order.
        add_test(NAME fluid-retract-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fluid_retract_test.py
                  $<TARGET_FILE:plusmc>)
        # The explosion half: a blast that opens a cell beside a pool owes it
        # a scheduled tick, and the water fills the crater.
        add_test(NAME explosion-fluid-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/explosion_fluid_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone G follow-up: drinking a potion — the potion_contents
        # component over the wire (given, decoded off the menu, and supplied by a
        # creative client), instant vs lasting effects, the glass bottle, and the
        # argument form's refusals.
        add_test(NAME potion-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/potion_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone G follow-up: brewing over the wire — the brewing stand menu
        # (41 slots, two data slots), three real brews (water bottles to awkward,
        # gunpowder to splash, dragon's breath to lingering), the has_bottle
        # block states, the level_event sound, the dropped crafting remainder
        # and the one-item cap on a bottle slot.
        add_test(NAME brewing-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/brewing_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone G follow-up: a thrown potion over the wire — the throw's
        # spawn packet, the bottle consumed, the flight, the break's level event,
        # the splash's falloff on the thrower and on a mob, and the lingering
        # cloud that keeps applying at a quarter duration.
        add_test(NAME thrown-potion-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/thrown_potion_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone G follow-up: fire over the real wire protocol —
        # flint and steel lighting real fire (and wearing out, not consumed),
        # the player catching fire and burning, and a thrown water bottle
        # dousing the block and extinguishing the player.
        add_test(NAME fire-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fire_test.py
                  $<TARGET_FILE:plusmc>)
        # The milestone G follow-up: the per-chunk block-tick loop — a real fire
        # that spreads into a flammable neighbour and burns the hay it was lit on
        # down, observed through nothing but the block updates the loop sends.
        add_test(NAME fire-spread-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/fire_spread_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone H: weather over the real wire protocol — /weather rain and
        # clear and the four game events, a player joining into a storm, the
        # rain putting a burning player out, a lit fire rained out, and a
        # cauldron filling.
        add_test(NAME weather-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/weather_test.py
                  $<TARGET_FILE:plusmc>)
        # The TNT follow-up over the real wire protocol — a fire priming a TNT
        # block, flint and steel priming one, the fuse counting down as
        # set_entity_data, the discard and the blast (block destruction, the
        # damage and knockback on a player in the blast, and the blast's own
        # explode packet with its registry ids, its 64-block audience and the
        # control client outside it) and two adjacent TNT blocks chaining with a
        # short fuse.
        add_test(NAME tnt-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/tnt_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone H: arrows and thrown items over the real wire protocol —
        # the bow's draw on the session clock and the 0.1 power gate, the
        # charge curve's two ends, the arrow's move-before-drag tick order, the
        # `minecraft:arrow` damage event and the armour step behind it, the
        # three throwables' fixed 1.5, and the ender pearl's teleport and 5.0.
        add_test(NAME projectile-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/projectile_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone J: the per-chunk random-tick pass over the real wire
        # protocol — a wheat crop growing through every age to maturity on
        # hydrated farmland (with a mature crop as the control), a lone
        # non-persistent leaf decaying, and a persistent leaf's distance
        # relaxing to a log placed beside it and back when the log goes.
        add_test(NAME random-tick-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/random_tick_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone I: mobs over the real wire protocol — /summon, a hostile
        # mob chasing and hitting the player, the player killing it for loot
        # and collected orbs, and /mobs reporting the population.
        add_test(NAME mobs
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mobs_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4's raid/boss slice, the HUD half: a summoned wither and ender
        # dragon add their UUID-keyed boss_event (title, full progress, the
        # type's vanilla colour), a joining client is replayed the live bars,
        # /mobs hurt moves a bar's progress, and /mobs kill removes them.
        add_test(NAME boss-bar
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/boss_bar_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4's wither encounter: the 220-tick spawn shield that refuses
        # damage and ends in the power-7 blast, the boss bar filling with it,
        # and the skull volley — Add Entity `minecraft:wither_skull` and its
        # `minecraft:wither_skull` damage event on the player.
        add_test(NAME wither-fight
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/wither_fight_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4's ender dragon: six crystals Add Entity at type 45 and heal
        # the bar, a sword swing destroys one in a power-6 blast, and the
        # half-health phase machine lands the dragon and takes it off again.
        add_test(NAME dragon-fight
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/dragon_fight_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4's raid: a pillager's omen, a villager consuming it into a
        # three-wave raid bar, the raider waves, and hero of the village.
        add_test(NAME raid
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/raid_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone I, roster half: every mob the generated table carries is
        # summoned over the real wire protocol and must arrive with the row's own
        # entity-type id, ring position and max_health — then despawn on /mobs
        # kill, so the whole 90-row table is exercised one entity at a time.
        add_test(NAME mob-roster
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_roster_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone I, breeding half: the right-click on an animal over the real
        # wire protocol — the range and food gates, setInLove and its heart
        # event, the held stack really shrinking, a birth (Add Entity + the
        # child's DATA_BABY_ID bit, the parents' orbs and heart burst), the
        # parents' 6000-tick cooldown refusing the next feeding, a baby fed up
        # and grown out of the bit by the natural tick, and — the follow-up — the
        # newborn watched for a long, pen-free window against two adults at
        # different distances: the one the child is not with killed with real
        # sword swings, and a fresh adult summoned seven and a half blocks past
        # the survivor — both held still only long enough to place and verify the
        # summon, then freed to stroll while the child follows (the test is about
        # a minute and a half).
        add_test(NAME breeding-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/breeding_test.py
                  $<TARGET_FILE:plusmc>)
        # The scene behind that birth, measured instead of inferred:
        # `BreedGoal.canUse` only looks eight blocks for a partner, and a cow in
        # love that finds none has no movement goal at all (the breed goal
        # outranks the stroll), so a pair that strolled apart before the feeding
        # can never breed. `breeding-wire` measures that gap and re-summons a
        # pair outside it; this soak reports how often the repair would be
        # needed in the first place — N trials of the same `/summon cow 2`, the
        # same ~5 s of free strolling the test's gate steps take, then the gap
        # off `/mobs list` — and fails only when a pair fed inside that margin
        # did not breed, or when a cow is not standing on the corridor floor. Its
        # first act is the containment proof: a summon from a stance whose ring
        # lands a cow in the rim column must come back on the floor (the unstuck
        # nudge), not on the rim's top. It is short (about two minutes) but
        # socket-timed, so it is serial and labelled `soak`: its out-of-range
        # rate is a measurement, and a box shared with three other servers would
        # move it.
        add_test(NAME breeding-scene-soak
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/breeding_scene_soak.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(breeding-scene-soak PROPERTIES
          LABELS "soak" TIMEOUT 600)
        # Phase 4 persistence: the mob population across a restart — two held
        # cows fed into love and paired (seen through /mobs list), a shutdown
        # that writes mobs.dat, a second boot on the same world dir that shows
        # the same entity ids, the same still-running love clocks and the same
        # reciprocal mate links, and a fresh summon that takes a brand-new id.
        add_test(NAME mob-persistence-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_persistence_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4 milestone L: villager trading on the wire — a summoned
        # villager's synched data, the merchant window's offers, a trade's
        # payment and reward, and the xp/level-up that follows.
        add_test(NAME villager-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/villager_test.py
                  $<TARGET_FILE:plusmc>)
        # The mob half of the entity tracking gate (docs/MOB_VISIBILITY.md): one
        # client summons a pig beside another, the second walks out of range and
        # back, and the add/remove/re-add sequence is read off the wire — then
        # the same script against PLUSMC_ENTITY_VISIBILITY=0, where none of those
        # transitions may happen. The gate's own red check: a gate whose absence
        # produces the same packets is a gate nobody has shown to do anything.
        add_test(NAME mob-visibility
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/mob_visibility_test.py
                  $<TARGET_FILE:plusmc>)
        # The item half of the same gate (docs/MOB_VISIBILITY.md): a dropped
        # stack is paired with both nearby clients, removed from the one that
        # walks out (after the item sweep's quarter second), paired again on the
        # way back, and a drop made while the other client is far reaches only
        # its dropper — then the same script against PLUSMC_ENTITY_VISIBILITY=0,
        # where none of those transitions may happen.
        add_test(NAME item-visibility
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/item_visibility_test.py
                  $<TARGET_FILE:plusmc>)
        # The rest of the entity relays on the same map (docs/MOB_VISIBILITY.md):
        # a primed TNT block and the orbs a blast leaves are paired only with the
        # client in range (walking in pairs, walking out removes), and an arrow
        # and a thrown bottle never reach a far client at all — then the same
        # scene against PLUSMC_ENTITY_VISIBILITY=0, where the far client must
        # see every one of them.
        add_test(NAME entity-relay-visibility
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/entity_relay_visibility_test.py
                  $<TARGET_FILE:plusmc>)
        # ...and the block relay's own audience, which is not a tracking range:
        # a block update reaches the clients that hold the chunk (the same square
        # stream_chunks sends and prunes on), so a client five chunks away gets
        # nothing for the cell and gets its updates back when it returns.
        add_test(NAME block-update-views
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/block_update_views_test.py
                  $<TARGET_FILE:plusmc>)
        # The arm-swing relay (`ClientboundAnimatePacket`, session.cpp's
        # handle_arm_animation): a tracked second client sees the swinger's main
        # and off hand as animate 0 and 3, the swinger receives no echo (vanilla's
        # broadcast excludes it), a hand byte outside `InteractionHand` sends
        # nothing, and walking out of the 48-block range ends the audience so the
        # next swing reaches nobody.
        add_test(NAME arm-swing-relay
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/arm_swing_relay_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4 milestone M: the scoreboard and the statistics screen over
        # the real protocol — an objective and its scores appear on a client
        # only once a display slot shows them, the six by-value criteria move
        # on the player tick, and mining/placing/a Q-drop move the item stats.
        add_test(NAME scoreboard-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoreboard_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # ...and the board across a restart: a `/scoreboard` objective, score,
        # display slot and team written by the shutdown save to
        # <world>/scoreboard.dat, and a second boot's joining client handed all
        # of them back by `send_initial_scoreboard`. The file codec is
        # tests/scoreboard_save_test.cpp; this is the session path.
        add_test(NAME scoreboard-persistence-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/scoreboard_persistence_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4 milestone N: advancements over the real protocol — a fresh
        # join is told nothing, the TICK trigger's one recipe helper arrives as
        # display-less nodes behind a reset, `/give cobblestone` reveals the
        # story branch with its DisplayInfo decoded field by field, the
        # already-visible root's completion is a progress-only flush, the
        # challenge's reward reaches totalExperience, and `seen_advancements`
        # drives `select_advancements_tab`.
        add_test(NAME advancement-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/advancement_wire_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 4 milestone N's persistence: the store across a restart — the
        # tick row and the story branch earned on the wire, an autosave and a
        # shutdown that write <world>/advancements.dat, a disconnect whose
        # progress has to outlive the session, and a second boot that hands each
        # player back exactly their own rows with their own instants.
        add_test(NAME advancement-persistence-wire
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/advancement_persistence_test.py
                  $<TARGET_FILE:plusmc>)
        # ...and the order that store (and the mob population beside it) has to
        # be read in: `load_mob_population` and `load_advancement_state` both
        # document that they must run before the listener starts, and a server
        # that opened the listener first served joins whose adoption found an
        # empty store — a rejoining player's restored rows were then missing for
        # the whole session. There is no client in this one: it reads the
        # server's own log up to the port line, which is the only place that
        # order is visible (the window closes long before a client could
        # connect) — booting twice, against an unreadable `mobs.dat` and
        # `advancements.dat` and then against a real store with a row and one
        # mob, so the loads that fail *and* the loads that succeed are both
        # pinned to the near side of the listener.
        add_test(NAME startup-order
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_order_test.py
                  $<TARGET_FILE:plusmc>)
        # ...and the socket's own backstop, which used to be a fixed 30 s while
        # the keep-alive beside it is tunable: a 60 s cadence can never be
        # answered inside 30 s, so a healthy-but-silent client was reaped
        # (advancement-persistence-wire's silent rejoin hit exactly that). The
        # derivation is logged at boot, so this needs no client and no 30 s
        # wait: three boots pin the default (30 s), a cadence above the floor
        # (60 s + 300 s -> 361 s), and a cadence below it (still 30 s).
        add_test(NAME socket-timeout
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/socket_timeout_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 6: the plugin API end to end. The example MODULE is loaded from a
        # temporary directory with its manifest, cancels a chat line, answers
        # /greet, reads the player/world accessors through /whereami, writes
        # blocks through /pillar and checks the client's block_update packets,
        # lists the player and a summoned mob through /entities, reads one back
        # by id with its velocity and facing through /entget (including a
        # diagonal move and a plugin teleport), loads a fixture module that
        # calls player_get and entity_get with the pre-velocity sizes (checks
        # below-floor structs are rejected, larger ones report the host's fill
        # size and keep their tail, the entities visitor's snapshots carry the
        # host's size, and a short pmc_item is refused) to pin the snapshot
        # size rules,
        # writes and reads a player-inventory slot through
        # /invset and /invget (checking the container_set_slot the owning session
        # sends), damages, teleports and despawns an entity through /entdmg,
        # /enttp and /entdespawn (checking the damage/hurt/health, position-sync
        # and Remove Entities packets, a lethal hit's death event with the tick's
        # loot and experience, and the queued player half), places and breaks a
        # block and denies each action through /blockguard (checking the event's
        # cell and states, the restored block state and the unconsumed stack),
        # ticks its scheduler, unloads on shutdown, and a second arm with a
        # wrong-ABI manifest must be skipped without stopping the server.
        # Inside the client-config branch because it joins a player.
        if(TARGET plusmc-plugin-greeter AND TARGET plusmc-plugin-oldabi
           AND TARGET plusmc-plugin-oldhost)
          add_test(NAME plugin-load
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/plugin_test.py
                    $<TARGET_FILE:plusmc>
                    $<TARGET_FILE:plusmc-plugin-greeter>
                    ${CMAKE_CURRENT_SOURCE_DIR}/examples/greeter/+mc-plugin.toml
                    $<TARGET_FILE:plusmc-plugin-oldabi>
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_player_plugin.toml
                    $<TARGET_FILE:plusmc-plugin-oldhost>
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/old_host_plugin.toml)
        endif()
        # Milestone D: commands /gamemode /tp /give /time (chat + chat_command paths).
        add_test(NAME commands
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/commands_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone E: persistence — place → save → reload byte-verified.
        add_test(NAME persistence
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/persistence_test.py
                  $<TARGET_FILE:plusmc>)
        # Milestone F: server.properties/eula/whitelist/ops + day-night cycle.
        add_test(NAME hosting
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/hosting_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 5 (ops): world snapshots, their rotation, and the restore drill
        # (`--backup-interval` / `--backup-keep` / `--restore-backup`,
        # docs/PHASE5.md §6 item 8). File-based on purpose: the assertions
        # compare the region file's bytes before and after the drill, so this is
        # deterministic rather than a load-sensitive timing test.
        add_test(NAME backup
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/backup_test.py
                  $<TARGET_FILE:plusmc>)
        # Phase 5 (ops): the *host-facing* restore command (`plusmc-restore`),
        # i.e. item 8's remainder — list snapshots, run the drill, and dry-run
        # it, without starting the server. Server-free on purpose (no listener,
        # no world open, no vanilla data), so it is fast and box-load independent.
        add_test(NAME restore-cli
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/restore_cli_test.py
                  $<TARGET_FILE:plusmc-restore>)
        # Phase 5 item 5 — perf as a CI gate. One idle server, no bots: startup
        # seconds to the `listening on` line, then a short window of the metrics
        # export, held against floors read from the checked-in baseline record
        # `bench/baseline/2026-10-15-i5-4570-perf-gate-idle.json` (a missing or
        # malformed record fails the gate rather than skipping it). The loaded
        # curve (movers, latency percentiles) needs the bot driver and a quiet
        # box and stays bench/soak.py's job — docs/PHASE5.md §5 says why. It is
        # labelled `bench` so a box that cannot hold even generous floors can
        # skip it with `ctest -LE bench`, and RUN_SERIAL so the other tests are
        # not starting their own servers into its measurement window.
        add_test(NAME perf-gate
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_gate_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(perf-gate PROPERTIES LABELS "bench" RUN_SERIAL TRUE)
        # Phase 5 item 0 — the loaded half of that gate. Sixteen clustered
        # movers, real protocol clients, a 12 s window: the tick cost against
        # PLAN §8's sustained ceiling and, load-independently, how many chunk
        # sends were served from bytes another connection built. That ratio is
        # the regression detector the idle arm cannot be: a crowded server that
        # stays at 20 TPS standing still is exactly the failure the idle gate
        # misses (docs/WRITE_PATH.md). Floors come from the checked-in
        # `bench/baseline/2026-10-17-i5-4570-perf-gate-loaded.json`; a missing
        # vanilla data root skips it (exit 77) because a client cannot join
        # without the client assets, and that is a missing input, not a pass.
        # Once the root is there, a box too loaded to join sixteen clients is a
        # failure instead: a harness limit is not a missing input.
        add_test(NAME perf-gate-loaded
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_gate_loaded_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(perf-gate-loaded PROPERTIES LABELS "bench"
                             RUN_SERIAL TRUE SKIP_RETURN_CODE 77 TIMEOUT 180)
        # Milestone H — the Tier-A gate (docs/PLAN.md §3.1). Not another
        # per-feature test: one session walks the whole Tier-A list in order
        # (join/spawn, movement, chat, blocks, day cycle, drops, inventories,
        # containers, crafting, furnaces, XP, death/respawn, commands), so a
        # regression that only appears when two features meet fails here. It is
        # labelled `tier-a` so CI can require it as a single, named gate.
        add_test(NAME tier-a-gate
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/tier_a_gate_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(tier-a-gate PROPERTIES LABELS "tier-a")
        # Phase 4's gate: the Tier-B list (docs/PLAN.md §3.1) walked in one
        # session — the tier-a-gate twin for mechanics depth. Labelled `tier-b`
        # so CI can require it as one named gate. It deliberately does not walk
        # "raid/boss basics": those encounters have their own wire tests
        # (`wither-fight`, `dragon-fight`, `raid`), and the gate's docstring says
        # so. The Tier-C ledger
        # (`tier-coverage`) is what measures the rest of the content surface.
        add_test(NAME tier-b-gate
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/tier_b_gate_test.py
                  $<TARGET_FILE:plusmc>)
        set_tests_properties(tier-b-gate PROPERTIES LABELS "tier-b")
        # The suite's long, load-sensitive wire tests: `tier-a-gate` and
        # `join-online` drive one long (or reconnecting) socket session,
        # `breeding-scene-soak` measures where free strolling takes a pair (a
        # number load would directly move), and `breeding-wire` watches a mob's
        # *world*-clock walk for most of a minute — a clock no test can turn into
        # packets. `projectile-wire` is
        # the same shape (83 s of draw-and-tick over one socket, with 1.2 s
        # drains that cannot be shortened without weakening the assertion);
        # under a packed `-j6` slice of this suite it missed packets and passed
        # alone immediately after. Run against the other
        # three servers of a packed slice, any of them can be starved past its
        # own poll deadlines and pass when re-run alone (`docs/PHASE4.md`, "Run
        # with `-j4` the two socket-heavy tests … can lose packets to machine
        # load"; and the loaded `-j4` runs behind this change). Their waits
        # already poll to a deadline, so the missing piece is not a longer wait
        # but not sharing four cores with three other servers. `RUN_SERIAL`
        # gives each of these the machine to itself while ctest still runs the
        # other ~93 tests in parallel, so the gate as a whole stays a `-j4` gate.
        # The Phase 4 boss-bar/wither/dragon wire tests are the same shape: each
        # starts a generated-world server and polls a short window (the dragon's
        # perch is 25 s), and all three missed their own deadlines in a packed
        # `-j4` run before passing alone immediately after.
        set_tests_properties(join-online tier-a-gate tier-b-gate breeding-wire
                             projectile-wire breeding-scene-soak
                             boss-bar wither-fight dragon-fight
                             PROPERTIES RUN_SERIAL TRUE)

        # The load-stability proof (docs/PHASE3.md): every socket test at once,
        # three times, with each run's log kept and each test's printed steps
        # compared between repeats. Deliberately NOT `add_test`: it is minutes
        # long and is meant to be asked on purpose, not on every gate.
        #
        #   cmake --build build --target load-stability
        #
        # The target takes no arguments, because Ninja does not forward `--`
        # extras to a custom command. Anything else the harness does — more
        # repeats, a `--filter` to hammer one suspicious test — is the script
        # called directly, which is how the docs quote its runs:
        #   python3 tests/load_stability.py build --repeats 6 --filter '^mobs$'
        add_custom_target(load-stability
          COMMAND ${Python3_EXECUTABLE}
                  ${CMAKE_CURRENT_SOURCE_DIR}/tests/load_stability.py
                  ${CMAKE_BINARY_DIR} --repeats 3
          WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
          USES_TERMINAL)
      endif()
    endif()
  else()
    message(STATUS "Python3 not found — skipping Python-based tests")
  endif()
endif()
