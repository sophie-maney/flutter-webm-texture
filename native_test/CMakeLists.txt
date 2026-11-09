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

add_executable(wt_native_tests
  $<TARGET_OBJECTS:wt_c_abi_check>
  api_test.cc
  deps_test.cc
  sanitizer_test.cc
)
target_link_libraries(wt_native_tests PRIVATE wt_core GTest::gtest_main)
target_compile_options(wt_native_tests PRIVATE -Wall -Wextra $<$<BOOL:${WT_WERROR}>:-Werror>)

include(GoogleTest)
gtest_discover_tests(wt_native_tests DISCOVERY_MODE PRE_TEST)
