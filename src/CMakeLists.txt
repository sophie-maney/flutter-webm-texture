# The webm_texture core: a static library behind the C ABI in api/wt_api.h.
#
# Included by the root CMakeLists.txt for host builds (tests, fuzzing). The
# Android plugin build adds this directory too (T1.8). iOS compiles the same
# sources through SwiftPM/CocoaPods against the xcframework instead.
#
# Inputs (set before add_subdirectory):
#   WT_DEPS_DIR   host only: prefix from tool/build_host_deps.sh
#   WT_SANITIZE   compile with ASan + UBSan

# --- Third-party deps -------------------------------------------------------
# One INTERFACE target, wt_deps, carries the include path and the static libs.
set(WT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
add_library(wt_deps INTERFACE)

if(ANDROID)
  # Prebuilts from tool/fetch_prebuilts.dart: every library merged into one
  # archive per ABI.
  set(_wt_prebuilt "${WT_REPO_ROOT}/third_party/prebuilt/android")
  set(_wt_archive "${_wt_prebuilt}/${ANDROID_ABI}/libwebm_texture_deps.a")
  if(NOT EXISTS "${_wt_archive}")
    message(FATAL_ERROR "${_wt_archive} is missing. Run: fvm dart run tool/fetch_prebuilts.dart")
  endif()
  target_include_directories(wt_deps SYSTEM INTERFACE "${_wt_prebuilt}/include")
  target_link_libraries(wt_deps INTERFACE "${_wt_archive}" m)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  message(FATAL_ERROR "iOS builds the core through ios/webm_texture (SwiftPM/CocoaPods), not CMake")
else()
  # Linking order matters for static archives: users before what they use.
  set(_wt_host_libs webpdemux webpdecoder vpx yuv gif lz4)
  foreach(_lib IN LISTS _wt_host_libs)
    set(_path "${WT_DEPS_DIR}/lib/lib${_lib}.a")
    if(NOT EXISTS "${_path}")
      message(FATAL_ERROR "${_path} is missing. Run: tool/build_host_deps.sh "
                          "(or point -DWT_DEPS_DIR at its prefix)")
    endif()
    target_link_libraries(wt_deps INTERFACE "${_path}")
  endforeach()
  find_package(Threads REQUIRED)
  target_include_directories(wt_deps SYSTEM INTERFACE "${WT_DEPS_DIR}/include")
  target_link_libraries(wt_deps INTERFACE Threads::Threads)
  if(NOT APPLE)
    target_link_libraries(wt_deps INTERFACE m)
  endif()
endif()

# --- Core -------------------------------------------------------------------
add_library(wt_core STATIC
  api/wt_api.cc
)
# Consumers include "wt_api.h"; core sources include "api/wt_api.h" etc.
target_include_directories(wt_core
  PUBLIC "${CMAKE_CURRENT_LIST_DIR}/api"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(wt_core PUBLIC cxx_std_17)
set_target_properties(wt_core PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
# AGENTS.md: no exceptions or RTTI in the core. UBSan's vptr check needs RTTI,
# so it is off here (it stays on in the tests).
target_compile_options(wt_core PRIVATE
  -fno-exceptions -fno-rtti
  -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${WT_SANITIZE}>:-fno-sanitize=vptr>
  $<$<BOOL:${WT_WERROR}>:-Werror>)
target_link_libraries(wt_core PUBLIC wt_deps)
