# libFuzzer targets (WT_FUZZ=ON, LLVM clang). The root CMakeLists.txt already
# compiles everything with -fsanitize=fuzzer-no-link plus ASan + UBSan; only
# these executables link the libFuzzer main.
#
#   cmake -S . -B build/fuzz -DWT_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/fuzz -j
#   tool/fuzz.sh 60        # every target in parallel, with its seeds and dict
#
# tool/fuzz.sh knows each target's seed directories (fuzz/corpus/README.md).
# CI runs it for 60 s on PRs and 30 min nightly (fuzz-nightly.yml).
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
wt_add_fuzzer(fuzz_sniff fuzz_sniff.cc)
wt_add_fuzzer(fuzz_wsc fuzz_wsc.cc)

# Regenerates fuzz/corpus/wsc/ (see fuzz/corpus/README.md). Not a fuzzer.
add_executable(wsc_seeds wsc_seeds.cc)
target_link_libraries(wsc_seeds PRIVATE wt_core)
set_target_properties(wsc_seeds PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
