# Protocol fuzz targets (see plusmc top-level CMakeLists: PLUSMC_BUILD_FUZZERS).
#
# Two targets share fuzz/frame_reader_fuzz.cpp:
#   * fuzz-frame-standalone — same harness plus a driver main() that replays
#     corpus files and generates deterministic pseudo-random inputs. Builds
#     with any compiler, so it runs under ASan/UBSan even on toolchains whose
#     libFuzzer runtime cannot link (Apple clang).
#   * fuzz-frame-reader    — the real libFuzzer target (LLVMFuzzerTestOneInput).
#     Only added when the compiler accepts -fsanitize=fuzzer. Configure the
#     build dir with the sanitizers you want, e.g.
#       cmake -B build-fuzz -DPLUSMC_BUILD_FUZZERS=ON \
#             -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
#     then run:
#       cmake --build build-fuzz --target fuzz-frame-standalone
#       ./build-fuzz/fuzz/fuzz-frame-standalone fuzz/corpus/frame_reader/* --iters 100000
#     or, with a libFuzzer-capable clang:
#       cmake --build build-fuzz --target fuzz-frame-reader
#       ./build-fuzz/fuzz/fuzz-frame-reader fuzz/corpus/frame_reader/ -max_total_time=60
add_executable(fuzz-frame-standalone frame_reader_fuzz.cpp)
target_compile_definitions(fuzz-frame-standalone PRIVATE PLUSMC_FUZZ_STANDALONE=1)
target_link_libraries(fuzz-frame-standalone PRIVATE plusmc-core)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(fuzz-frame-standalone PRIVATE -g -O1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # A full compile+link probe: -fsanitize=fuzzer compiles on toolchains whose
  # fuzzer runtime is missing (e.g. Apple clang), so a compile-only flag check
  # would wrongly claim support and then fail at link time.
  include(CheckCXXSourceCompiles)
  set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
  check_cxx_source_compiles("int main() { return 0; }" PLUSMC_HAS_LIBFUZZER)
  unset(CMAKE_REQUIRED_FLAGS)
  if(PLUSMC_HAS_LIBFUZZER)
    add_executable(fuzz-frame-reader frame_reader_fuzz.cpp)
    target_link_libraries(fuzz-frame-reader PRIVATE plusmc-core)
    target_compile_options(fuzz-frame-reader PRIVATE -fsanitize=fuzzer -g -O1)
    target_link_options(fuzz-frame-reader PRIVATE -fsanitize=fuzzer)
    # Not part of the default build: CI builds it explicitly.
    set_target_properties(fuzz-frame-reader PROPERTIES EXCLUDE_FROM_ALL ON)
  else()
    message(STATUS "fuzz: compiler cannot link -fsanitize=fuzzer; "
                   "building fuzz-frame-standalone only")
  endif()
endif()
