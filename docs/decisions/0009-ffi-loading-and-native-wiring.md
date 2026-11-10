# 0009: How the core reaches Dart, and how each platform builds it

- **Status:** Accepted (T1.8)
- **Date:** 2026-11-10

## Context

T1.8 puts the C core behind `dart:ffi` and builds it into the Android and iOS plugins. Building it settled four things `PLAN.md` left open.

1. The Android prebuilt `libyuv` links against `__arm_get_current_vg` (from its SME code, `*_sme.cc`). Only NDK r30's compiler-rt provides it, and the prebuilts workflow builds with the newest NDK on its runner. The archive therefore fails to link under r28.2, Flutter's default.
2. SwiftPM can't read outside its package, and the core lives in `src/`, shared with Android.
3. `DynamicLibrary.process()` only finds symbols the linker kept, and a statically linked iOS app drops unreferenced exports.
4. Flutter treats CocoaPods as legacy.

## Decision

1. **The Android plugin requires NDK r30 or newer.** `android/build.gradle.kts` sets `ndkVersion = "30.0.16248370"`, and the example app matches it. AGP downloads it when missing. We considered rebuilding libyuv with `-DLIBYUV_DISABLE_SME` instead, which would work under r28, but chose not to give up the dependency on a current NDK in the build recipe. `build-android.sh` still accepts r28+, so prebuilts built there must come from r30 to link.
2. **Android** compiles the core into `libwebm_texture.so` from `android/CMakeLists.txt` (`add_subdirectory(../src)`), exporting only `wt_*` and aligned to 16 KB pages. Dart opens it by name.
3. **iOS is SwiftPM only; there is no podspec.** The core is compiled into the app (target `wt_core`) against the xcframework, and Dart uses `DynamicLibrary.process()`. The Swift plugin references every `wt_*` function from `register(with:)` so a static link keeps them.
4. **`ios/webm_texture/Sources/wt_core`** holds a symlink per `src/` subdirectory, plus `include/wt_api.h`, the only public header, so Swift sees the C ABI and not the C++ internals. A new `src/` subdirectory needs a new link; `test/native_wiring_test.dart` fails until it exists.
5. **Host tests of the Dart side** load a shared build of the core (`-DWT_BUILD_SHARED=ON`, `tool/build_host_ffi_lib.sh`) named by `WEBM_TEXTURE_LIB`. They skip when it is unset. The device integration test runs the same expectations (`example/integration_test/corpus_expectations.dart`).
6. **New packages:** `ffi` (runtime; allocation and UTF-8 for `dart:ffi`) and `ffigen` (dev; already named in `AGENTS.md`).

## Consequences

- Consumers need NDK r30 installed or downloadable.
- Dropping CocoaPods means apps that opt out of SwiftPM can't use the plugin.
- `ffigen` bindings stay in `lib/src/gen/`, regenerated with `fvm dart run ffigen --config ffigen.yaml`. The YAML config is deprecated upstream; migrate to the Dart generator API when it is removed.
