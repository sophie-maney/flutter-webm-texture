# AGENTS.md

Instructions for any coding agent working in this repository.

## What this is
`webm_texture` (name TBC) is a Flutter plugin for **Android and iOS only**. It plays animated stickers inline, the way Telegram does: VP9/VP8 WebM with alpha, animated WebP and GIF. A shared C++ core decodes in software, caches frames, and renders them into Flutter `Texture`s.

- **Why**: [`PLAN.md`](PLAN.md) holds the architecture, decisions and risks.
- **What to do next**: [`EXECUTION_PLAN.md`](EXECUTION_PLAN.md) lists the ordered tasks, each with acceptance criteria and a recommended model and effort level.

## Ground rules
1. **Always run Flutter/Dart through `fvm`** (`fvm flutter …`, `fvm dart …`). The SDK version is pinned in `.fvmrc`; never change it unless a task tells you to.
2. **Work on exactly one task from `EXECUTION_PLAN.md` at a time**, on its own branch named `task/<ID>-<slug>` (e.g. `task/T1.3-webm-demuxer`). Don't start a task until its dependencies are ticked.
3. **A task is done only when every acceptance check passes.** Run them, and paste the command output summary into the PR description. Then tick the task's checkbox in `EXECUTION_PLAN.md` in the same PR.
4. **Stop at 🛑 gates.** Spike tasks end with a written report in `docs/spikes/` and a human decision. Don't make that decision yourself, and don't start dependent tasks.
5. **Decisions go in writing.** Any deviation from `PLAN.md` (library choice, API shape, file format) gets a short ADR in `docs/decisions/NNNN-title.md`, and `PLAN.md` is updated in the same PR.
6. **Stay in scope.** Don't add desktop/web platforms, encoding, audio, Lottie/TGS, or new dependencies that aren't in `PLAN.md` without an ADR.

## Repository map
```
lib/                 Dart public API (lib/webm_texture.dart) + lib/src/ internals
pigeons/             Pigeon definitions → generated code in lib/src/gen, android/, ios/
src/                 C++17 core (demux/ decode/ convert/ cache/ sched/ atlas/ api/)
  api/wt_api.h       The ONLY C ABI surface (FFI + platform glue include this)
third_party/         versions.lock, build scripts; prebuilt/ is gitignored
android/             Kotlin plugin + JNI + CMake
ios/webm_texture/    Package.swift + .podspec, Swift + ObjC++ shim
native_test/         GoogleTest host tests
fuzz/                libFuzzer targets + seed corpus
test/                Dart unit/widget tests
test_assets/         Generated corpus (tool/gen_corpus.sh) + reference frames
example/             Demo app + integration_test/
tool/                fetch_prebuilts.dart, gen_corpus.sh, bench scripts
docs/                spikes/, decisions/, cache-format.md
```

## Commands
Each command works once the task that introduces it has landed (that task's ID is in brackets).

| Purpose | Command |
|---|---|
| Deps | `fvm flutter pub get` |
| Format | `fvm dart format --set-exit-if-changed .` · `clang-format --dry-run -Werror $(git ls-files 'src/*.cc' 'src/*.h')` |
| Analyze | `fvm flutter analyze` |
| Dart tests | `fvm flutter test` |
| Build iOS prebuilts [T0.4] | `third_party/build/build-ios.sh` (macOS + Xcode, `brew install cmake nasm`) → `third_party/prebuilt/ios/WebmTextureDeps.xcframework` |
| Build Android prebuilts [T0.5] | `third_party/build/build-android.sh [--with-x86]` (NDK r28+, `cmake`, `nasm`) → `third_party/prebuilt/android/{include,<abi>/libwebm_texture_deps.a}` |
| Third-party licenses [T0.4] | `third_party/build/collect-licenses.sh` → `third_party/LICENSES.md` (commit it) |
| Fetch native libs [T0.6] | `fvm dart run tool/fetch_prebuilts.dart` |
| Native host tests [T1.1] | `cmake -S . -B build/host -DWT_HOST_TESTS=ON -DWT_SANITIZE=ON && cmake --build build/host -j && ctest --test-dir build/host --output-on-failure` |
| Fuzz (60 s smoke) [T1.7] | `cmake -S . -B build/fuzz -DWT_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++ && cmake --build build/fuzz -j && build/fuzz/fuzz_webm -max_total_time=60 fuzz/corpus/webm` |
| Regenerate bindings | `fvm dart run pigeon --input pigeons/messages.dart` · `fvm dart run ffigen --config ffigen.yaml` |
| Regenerate corpus [T0.3] | `tool/gen_corpus.sh` (needs `ffmpeg` with libvpx + libopus, `img2webp`, `gif2webp`, `gifsicle`, `jq`) · validate: `tool/gen_corpus.sh --check` |
| Android build | `cd example && fvm flutter build apk --debug` |
| iOS build | `cd example && fvm flutter build ios --simulator --debug` |
| Integration tests | `cd example && fvm flutter test integration_test -d <device>` |

## Conventions
- **Dart**: `flutter_lints` plus the rules in `analysis_options.yaml`. Public API is documented with dartdoc. Don't export anything from `lib/src/` unless the API in `PLAN.md` lists it.
- **C++17**: Google style via `.clang-format`. Build with `-fno-exceptions -fno-rtti` and use no exceptions across the C ABI; return `wt_status` codes.
  - Every allocation is bounded by `StickerLimits`, and every buffer write is bounds-checked.
  - Use RAII inside the core. `wt_api.h` stays plain C with explicit `*_create`/`*_destroy` pairs.
- **Kotlin/Swift**: keep platform glue thin (texture plumbing, lifecycle, device-tier signals). All media logic lives in `src/`.
- **Threads**: never decode on the platform main thread or the Flutter UI/raster threads. Document the thread affinity of every `wt_api.h` function in its header comment.
- **Pixel formats**: frames are **premultiplied**. Android writes RGBA_8888 and iOS writes BGRA (in libyuv naming, `ABGR` and `ARGB` respectively).
- **Generated code** gets committed and must not be edited by hand. Regenerate it with the commands above.
- **Commits**: imperative subject ≤ 72 chars, with the task ID in the body (e.g. `Task: T1.3`).

## Security
Sticker files come from other users, so treat every input as hostile.
- Never weaken `StickerLimits` defaults or libvpx's `--size-limit` without an ADR.
- Any new parser or decoder path needs a fuzz target in the same PR.
- Host tests run under ASan/UBSan. A sanitizer finding blocks merge.

## Native libraries
- Versions and sha256s live only in `third_party/versions.lock`. Bump them via a PR that reruns the prebuilt workflow.
- The release tag `deps-<lockhash>` hashes only the `versions.lock` rows. After changing a build script or flag without touching the lock, run the Prebuilts workflow manually with `force` so the release is rebuilt.
- Never commit binaries. `third_party/prebuilt/` is gitignored and filled by `tool/fetch_prebuilts.dart`.
- Android `.so` files must be 16 KB page aligned. Check with `llvm-readelf -l <so> | grep LOAD`, where alignment must be `0x4000`.

## Devices
- Prefer the Android emulator (API 24 and the latest) and the iOS Simulator (iOS 15 and the latest).
- Tasks marked **needs real device** (transparency and performance spikes, benchmarks) must say which device was used. If you have no device, stop and ask; don't substitute an emulator silently.
