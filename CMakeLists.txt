# Host build of the webm_texture core: GoogleTest suite and libFuzzer targets.
#
#   tool/build_host_deps.sh
#   cmake -S . -B build/host -DWT_HOST_TESTS=ON -DWT_SANITIZE=ON
#   cmake --build build/host -j
#   ctest --test-dir build/host --output-on-failure
#
# Thread check of the scheduler (TSan; can't be combined with WT_SANITIZE):
#   cmake -S . -B build/tsan -DWT_HOST_TESTS=ON -DWT_TSAN=ON
#   cmake --build build/tsan -j && ctest --test-dir build/tsan -R 'Sched|Timeline|SkipFrame|SanitizerTest|Atlas' --output-on-failure
#
# The plugin itself doesn't use this file: Android adds src/ from
# android/CMakeLists.txt, and iOS builds the sources through SwiftPM.
cmake_minimum_required(VERSION 3.22)
project(webm_texture_core LANGUAGES C CXX)

option(WT_HOST_TESTS "Build the GoogleTest suite in native_test/" OFF)
option(WT_SANITIZE "Build everything with ASan + UBSan" OFF)
option(WT_TSAN "Build everything with ThreadSanitizer (not with WT_SANITIZE)" OFF)
option(WT_FUZZ "Build the libFuzzer targets in fuzz/ (clang only; implies ASan + UBSan)" OFF)
option(WT_BUILD_SHARED "Build libwebm_texture (shared) so Dart tests can load it" OFF)
option(WT_WERROR "Treat warnings in our own code as errors" OFF)
set(WT_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/host-core/prefix"
    CACHE PATH "Prefix written by tool/build_host_deps.sh")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(WT_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR "WT_FUZZ needs LLVM clang with libFuzzer (Apple clang has none), "
                        "e.g. -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++")
  endif()
  set(WT_SANITIZE ON)
  # Instrument all code for coverage; only the fuzz executables link the
  # libFuzzer main (see fuzz/CMakeLists.txt).
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# Sanitizers apply to everything compiled here (core, GoogleTest, tests), so
# ASan sees every allocation we make. The prebuilt deps are not instrumented.
if(WT_SANITIZE)
  set(_wt_san -fsanitize=address,undefined -fno-sanitize-recover=all)
  add_compile_options(${_wt_san} -fno-omit-frame-pointer -g)
  add_link_options(${_wt_san})
  add_compile_definitions(WT_SANITIZE=1)
endif()

if(WT_TSAN)
  if(WT_SANITIZE)
    message(FATAL_ERROR "WT_TSAN and WT_SANITIZE (ASan) can't be combined")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
  add_compile_definitions(WT_TSAN=1)
endif()

add_subdirectory(src)

if(WT_HOST_TESTS)
  enable_testing()
  add_subdirectory(native_test)
endif()

if(WT_FUZZ)
  add_subdirectory(fuzz)
endif()
