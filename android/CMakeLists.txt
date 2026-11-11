# Builds libwebm_texture.so: the C++ core (src/) behind the C ABI in
# src/api/wt_api.h, linked against the prebuilt deps, plus the JNI texture
# glue in src/main/cpp (T2.2). Dart loads it with
# DynamicLibrary.open('libwebm_texture.so') and Kotlin with
# System.loadLibrary("webm_texture"), so both share one copy of the core. Run
# `fvm dart run tool/fetch_prebuilts.dart` first.
cmake_minimum_required(VERSION 3.22)
project(webm_texture_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/core")

# The glue only sees wt_api.h (through the core target's public include) and
# exports only its JNIEXPORT functions; the core's visibility settings apply.
target_sources(webm_texture PRIVATE
  src/main/cpp/atlas_window.cc
  src/main/cpp/jni_bridge.cc
  src/main/cpp/surface_player.cc)
target_link_libraries(webm_texture PRIVATE android log)
