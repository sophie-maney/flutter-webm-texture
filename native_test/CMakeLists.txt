# GoogleTest host suite for the core (T1.1+).
#
# GoogleTest is compiled from the pinned sources that tool/build_host_deps.sh
# unpacks (third_party/test-deps.lock), with the same sanitizer flags as
# everything else in the host build.
set(_wt_gtest_src "${WT_DEPS_DIR}/src/googletest")
if(NOT EXISTS "${_wt_gtest_src}/CMakeLists.txt")
  message(FATAL_ERROR "${_wt_gtest_src} is missing. Run: tool/build_host_deps.sh")
endif()
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${_wt_gtest_src}" "${CMAKE_CURRENT_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL SYSTEM)

# The header must compile as strict C, since Dart (ffigen) and the ObjC/JNI
# glue consume it as C. This file also pins struct layouts for ffigen.
add_library(wt_c_abi_check OBJECT c_abi_check.c)
target_link_libraries(wt_c_abi_check PRIVATE wt_core)
set_target_properties(wt_c_abi_check PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(wt_c_abi_check PRIVATE -Wall -Wextra -Wpedantic -Werror)

# zlib reads the gzipped reference frames (ADR 0001). Host tests only; the
# plugin never links it.
find_package(ZLIB REQUIRED)

add_executable(wt_native_tests
  $<TARGET_OBJECTS:wt_c_abi_check>
  anim_test.cc
  api_test.cc
  atlas_stress_test.cc
  atlas_test.cc
  cache_test.cc
  decode_test.cc
  deps_test.cc
  golden_test.cc
  manifest.cc
  probe_test.cc
  ref_frames.cc
  sanitizer_test.cc
  sched_stress_test.cc
  sched_test.cc
  slot_allocator_test.cc
  source_test.cc
  store_test.cc
  webm_test.cc
)
target_link_libraries(wt_native_tests PRIVATE wt_core GTest::gtest_main ZLIB::ZLIB)
# Internal headers (demux/…) are tested directly, besides the public C ABI.
target_include_directories(wt_native_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(wt_native_tests PRIVATE
  WT_TEST_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../test_assets")
target_compile_options(wt_native_tests PRIVATE -Wall -Wextra $<$<BOOL:${WT_WERROR}>:-Werror>)

include(GoogleTest)
gtest_discover_tests(wt_native_tests DISCOVERY_MODE PRE_TEST)
