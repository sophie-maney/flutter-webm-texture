# libFuzzer targets (WT_FUZZ=ON, LLVM clang). The root CMakeLists.txt already
# compiles everything with -fsanitize=fuzzer-no-link plus ASan + UBSan; only
# these executables link the libFuzzer main.
#
#   cmake -S . -B build/fuzz -DWT_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/fuzz -j
#   mkdir -p build/fuzz/corpus/probe build/fuzz/corpus/webm
#   build/fuzz/fuzz_probe -max_total_time=60 build/fuzz/corpus/probe test_assets
#   build/fuzz/fuzz_webm -max_total_time=60 -dict=fuzz/webm.dict \
#     build/fuzz/corpus/webm fuzz/corpus/webm
#
# libFuzzer writes new inputs into the first corpus directory, so committed
# seeds (fuzz/corpus/, test_assets/) always go after a scratch directory.
#
# T1.1 added the wiring and fuzz_probe, T1.3 fuzz_webm's demuxing and T1.4
# its decoding. T1.7 adds fuzz_sniff and owns the timed and nightly CI runs.
function(wt_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE wt_core)
  # Internal headers (demux/…) are fuzzed directly, besides the C ABI.
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  # Put the binaries at build/fuzz/<name>, the path AGENTS.md documents.
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

wt_add_fuzzer(fuzz_probe fuzz_probe.cc)
wt_add_fuzzer(fuzz_webm fuzz_webm.cc)
