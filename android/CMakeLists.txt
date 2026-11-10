# Builds libwebm_texture.so: the C++ core (src/) behind the C ABI in
# src/api/wt_api.h, linked against the prebuilt deps. Dart loads it with
# DynamicLibrary.open('libwebm_texture.so'). Run
# `fvm dart run tool/fetch_prebuilts.dart` first.
cmake_minimum_required(VERSION 3.22)
project(webm_texture_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../src" "${CMAKE_CURRENT_BINARY_DIR}/core")
