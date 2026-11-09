# Spike A (T0.7) host build. Throwaway; T1.1 owns the real CMake.
#
#   spikes/alpha/build-host-deps.sh
#   cmake -S spikes/alpha -B build/spike-alpha -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/spike-alpha -j
#
# -DSPIKE_SANITIZE=ON builds everything we compile (the CLI, both demuxers and
# webm_parser itself, with asserts on) under ASan + UBSan. libvpx and libyuv
# come prebuilt from build/deps/host/prefix without sanitizers.
cmake_minimum_required(VERSION 3.20)
project(alpha_spike C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(WT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(DEPS "${WT_ROOT}/build/deps/host/prefix" CACHE PATH "host deps prefix")
set(LIBWEBM_SRC "${WT_ROOT}/build/deps/src/libwebm" CACHE PATH "libwebm source")
option(SPIKE_SANITIZE "Build with ASan + UBSan" OFF)

foreach(lib libvpx.a libyuv.a)
  if(NOT EXISTS "${DEPS}/lib/${lib}")
    message(FATAL_ERROR "${DEPS}/lib/${lib} missing; run spikes/alpha/build-host-deps.sh")
  endif()
endforeach()
find_package(ZLIB REQUIRED)

# Production rules from AGENTS.md: no exceptions, no RTTI.
set(SPIKE_FLAGS -fno-exceptions -fno-rtti)
if(SPIKE_SANITIZE)
  list(APPEND SPIKE_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

# webm_parser from the pinned libwebm source, compiled with our flags so the
# sanitizers (and its asserts, in the sanitizer build) cover it.
file(GLOB WEBM_PARSER_SRCS "${LIBWEBM_SRC}/webm_parser/src/*.cc")
add_library(webm_parser_spike STATIC ${WEBM_PARSER_SRCS})
target_include_directories(webm_parser_spike
  PUBLIC "${LIBWEBM_SRC}/webm_parser/include"
  PRIVATE "${LIBWEBM_SRC}/webm_parser")
target_compile_options(webm_parser_spike PRIVATE ${SPIKE_FLAGS})
if(SPIKE_SANITIZE)
  target_compile_options(webm_parser_spike PRIVATE -UNDEBUG)
endif()

add_executable(alpha_spike alpha_spike.cc demux_libwebm.cc demux_mini_ebml.cc)
target_include_directories(alpha_spike PRIVATE "${DEPS}/include")
target_compile_options(alpha_spike PRIVATE ${SPIKE_FLAGS} -Wall -Wextra)
target_link_libraries(alpha_spike PRIVATE webm_parser_spike
  "${DEPS}/lib/libvpx.a" "${DEPS}/lib/libyuv.a" ZLIB::ZLIB)

# Code-size probes: the same tiny driver linked against each demuxer, built
# -Os with dead stripping, to compare what each adds to a binary.
set(SIZE_FLAGS -Os -DNDEBUG -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
if(APPLE)
  set(SIZE_LINK -Wl,-dead_strip)
else()
  set(SIZE_LINK -Wl,--gc-sections)
endif()
add_library(webm_parser_os STATIC ${WEBM_PARSER_SRCS})
target_include_directories(webm_parser_os
  PUBLIC "${LIBWEBM_SRC}/webm_parser/include"
  PRIVATE "${LIBWEBM_SRC}/webm_parser")
target_compile_options(webm_parser_os PRIVATE ${SIZE_FLAGS})
foreach(probe none libwebm mini)
  set(srcs size_probe.cc)
  if(probe STREQUAL "libwebm")
    list(APPEND srcs demux_libwebm.cc)
  elseif(probe STREQUAL "mini")
    list(APPEND srcs demux_mini_ebml.cc)
  endif()
  add_executable(size_${probe} ${srcs})
  string(TOUPPER "${probe}" up)
  target_compile_definitions(size_${probe} PRIVATE PROBE_${up})
  target_compile_options(size_${probe} PRIVATE ${SIZE_FLAGS})
  target_link_options(size_${probe} PRIVATE ${SIZE_LINK})
  if(probe STREQUAL "libwebm")
    target_link_libraries(size_${probe} PRIVATE webm_parser_os)
  endif()
endforeach()
