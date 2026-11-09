# libFuzzer targets (WT_FUZZ=ON, LLVM clang). The root CMakeLists.txt already
# compiles everything with -fsanitize=fuzzer-no-link plus ASan + UBSan; only
# these executables link the libFuzzer main.
#
#   cmake -S . -B build/fuzz -DWT_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/fuzz -j
#   build/fuzz/fuzz_probe -max_total_time=60 test_assets
#
# T1.1 adds the wiring and fuzz_probe; T1.3 and T1.7 add fuzz_webm and
# fuzz_sniff, seed corpora, and the CI runs.
function(wt_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE wt_core)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  # Put the binaries at build/fuzz/<name>, the path AGENTS.md documents.
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

wt_add_fuzzer(fuzz_probe fuzz_probe.cc)
