#!/usr/bin/env bash
# Builds the iOS native dependencies into one static xcframework:
#
#   <out>/WebmTextureDeps.xcframework
#     ios-arm64/                    libWebmTextureDeps.a + Headers/
#     ios-arm64_x86_64-simulator/   libWebmTextureDeps.a + Headers/
#
# Every library in third_party/versions.lock (libvpx, libwebm, libyuv,
# libwebp, giflib, lz4) is merged into the one archive, and its public headers
# are included. Sources are fetched and sha256-verified by common.sh.
#
# Usage: third_party/build/build-ios.sh [--out DIR] [--jobs N]
#   --out   output directory (default: third_party/prebuilt/ios)
#   --jobs  parallel compile jobs (default: CPU count)
#
# Needs macOS with Xcode, plus cmake and nasm (`brew install cmake nasm`);
# nasm assembles libvpx's x86_64 simulator code.

set -euo pipefail

# shellcheck source=common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

IOS_MIN="15.0"
OUT="$WT_ROOT/third_party/prebuilt/ios"
JOBS="$(wt_ncpu)"
FRAMEWORK="WebmTextureDeps"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h | --help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) wt_die "unknown argument '$1' (see --help)" ;;
  esac
done
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ "$(uname -s)" = "Darwin" ] || wt_die "iOS libraries can only be built on macOS"
wt_require xcrun xcodebuild lipo libtool cmake nasm make perl curl git shasum tar
xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 ||
  wt_die "the iphoneos SDK is missing; install Xcode and run xcode-select"

IOS_WORK="$WT_WORK/ios"

# ---------------------------------------------------------------------------
# Sources

for lib in $WT_LIBS; do
  wt_fetch_source "$lib"
done

# ---------------------------------------------------------------------------
# Per-slice builds. Each slice installs into $IOS_WORK/<slice>/prefix.

build_libvpx() {
  local vpx_target extra=""
  case "$SLICE" in
    # libvpx has no arm64 simulator target, and its iOS device target
    # (arm64-darwin-gcc) ignores $CC in favour of xcrun's clang and pins iOS
    # 7.0. Both arm64 slices therefore use the generic arm64 macOS target, and
    # the compiler wrappers below retarget it to iOS.
    ios-arm64 | ios-sim-arm64) vpx_target="arm64-darwin23-gcc" ;;
    ios-sim-x86_64)
      vpx_target="x86_64-iphonesimulator-gcc"
      extra="--as=nasm --enable-runtime-cpu-detect"
      ;;
  esac

  local dir="$SLICE_WORK/libvpx"
  rm -rf "$dir" && mkdir -p "$dir/bin"

  # libvpx's darwin targets add their own -isysroot, -arch and an old
  # -m*-version-min (7.0) to compile and link lines alike. These wrappers drop
  # the version-min flags, which would otherwise beat the version in our
  # -target, and append our triple and sysroot last so they win. They also
  # silence the remaining override warnings, which would otherwise trip
  # configure's -Werror probes.
  local compiler name
  for name in cc cxx; do
    compiler="$CC"
    [ "$name" = cxx ] && compiler="$CXX"
    printf '%s\n' \
      '#!/bin/sh' \
      'for a in "$@"; do' \
      '  shift' \
      '  case "$a" in' \
      '    -miphoneos-version-min=* | -mios-simulator-version-min=* | -mmacosx-version-min=*) ;;' \
      '    *) set -- "$@" "$a" ;;' \
      '  esac' \
      'done' \
      "exec \"$compiler\" \"\$@\" -Wno-overriding-option $TARGET_FLAGS" >"$dir/bin/$name"
    chmod +x "$dir/bin/$name"
  done

  # nasm writes Mach-O objects without an LC_BUILD_VERSION, which makes the
  # app's linker warn about every one. This nasm shim relinks each object with
  # `ld -r` to stamp it as iOS Simulator; it shadows the real nasm on PATH.
  local path="$PATH"
  if [ "$ARCH" = x86_64 ]; then
    local sdk_version
    sdk_version="$(xcrun --sdk "$SDK" --show-sdk-version)"
    printf '%s\n' \
      '#!/bin/sh' \
      'out=""; prev=""' \
      'for a in "$@"; do [ "$prev" = "-o" ] && out="$a"; prev="$a"; done' \
      "\"$(command -v nasm)\" \"\$@\" || exit \$?" \
      '[ -n "$out" ] || exit 0' \
      "\"$(xcrun --sdk "$SDK" -f ld)\" -r -arch x86_64 -platform_version ios-simulator $IOS_MIN $sdk_version \\" \
      '  "$out" -o "$out.stamped" 2>/dev/null || { rm -f "$out.stamped"; exit 1; }' \
      'mv "$out.stamped" "$out"' >"$dir/bin/nasm"
    chmod +x "$dir/bin/nasm"
    path="$dir/bin:$PATH"
  fi

  # EXECUTION_PLAN.md lists --disable-encoders, but libvpx's configure has no
  # such switch; the per-codec --disable-vp{8,9}-encoder flags are the
  # equivalent.
  # shellcheck disable=SC2086
  (cd "$dir" &&
    PATH="$path" CC="$dir/bin/cc" CXX="$dir/bin/cxx" LD="$dir/bin/cc" AR="$AR" STRIP="$STRIP" \
      "$WT_SRC/libvpx/configure" \
      --target="$vpx_target" \
      --prefix="$PREFIX" \
      --extra-cflags="$COMMON_FLAGS" \
      --extra-cxxflags="$COMMON_FLAGS -fvisibility-inlines-hidden" \
      --disable-vp8-encoder \
      --disable-vp9-encoder \
      --enable-vp8-decoder \
      --enable-vp9-decoder \
      --disable-vp9-highbitdepth \
      --size-limit=1024x1024 \
      --enable-pic \
      --disable-examples \
      --disable-tools \
      --disable-docs \
      --disable-unit-tests \
      --enable-static \
      --disable-shared \
      --disable-libyuv \
      --disable-webm-io \
      --disable-install-bins \
      --disable-install-srcs \
      $extra) >"$dir/configure.log" 2>&1 ||
    { tail -30 "$dir/configure.log" >&2; wt_die "libvpx configure failed ($SLICE)"; }
  PATH="$path" make -C "$dir" -j"$JOBS" >"$dir/build.log" 2>&1 ||
    { tail -40 "$dir/build.log" >&2; wt_die "libvpx build failed ($SLICE)"; }
  make -C "$dir" install >>"$dir/build.log" 2>&1 ||
    { tail -20 "$dir/build.log" >&2; wt_die "libvpx install failed ($SLICE)"; }
}

# cmake_build <name> <source dir> [cmake args...]
cmake_build() {
  local name="$1" src="$2"
  shift 2
  local dir="$SLICE_WORK/$name"
  rm -rf "$dir"
  cmake -S "$src" -B "$dir" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_SYSTEM_PROCESSOR="$ARCH" \
    -DCMAKE_OSX_SYSROOT="$SDK" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN" \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="$CMAKE_CFLAGS" \
    -DCMAKE_CXX_FLAGS="$CMAKE_CFLAGS -fvisibility-inlines-hidden" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF \
    "$@" >"$dir.configure.log" 2>&1 ||
    { tail -30 "$dir.configure.log" >&2; wt_die "$name cmake configure failed ($SLICE)"; }
}

cmake_make() {
  local name="$1"
  shift
  cmake --build "$SLICE_WORK/$name" -j "$JOBS" "$@" >"$SLICE_WORK/$name.build.log" 2>&1 ||
    { tail -40 "$SLICE_WORK/$name.build.log" >&2; wt_die "$name build failed ($SLICE)"; }
}

build_libwebm() {
  cmake_build libwebm "$WT_SRC/libwebm" \
    -DENABLE_WEBM_PARSER=ON \
    -DENABLE_WEBMTS=OFF \
    -DENABLE_WEBMINFO=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_SAMPLE_PROGRAMS=OFF
  # Build only the library: the webm_parser demo is an executable, which an
  # iOS cross build can't link without code signing.
  cmake_make libwebm --target webm
  cmake --install "$SLICE_WORK/libwebm" >>"$SLICE_WORK/libwebm.build.log" 2>&1 ||
    wt_die "libwebm install failed ($SLICE)"
}

build_libyuv() {
  cmake_build libyuv "$WT_SRC/libyuv" -DLIBYUV_DISABLE_JPEG=ON -DUNIT_TEST=OFF
  # The `yuv` target is the static library; skip the shared lib and tools.
  cmake_make libyuv --target yuv
  mkdir -p "$PREFIX/lib" "$PREFIX/include"
  cp "$SLICE_WORK/libyuv/libyuv.a" "$PREFIX/lib/"
  cp -R "$WT_SRC/libyuv/include/." "$PREFIX/include/"
}

build_libwebp() {
  # Decode-only: libwebpdecoder (no encoder) plus libwebpdemux for
  # WebPAnimDecoder.
  cmake_build libwebp "$WT_SRC/libwebp" \
    -DWEBP_BUILD_ANIM_UTILS=OFF \
    -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_LIBWEBPMUX=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF
  cmake_make libwebp --target webpdecoder webpdemux
  mkdir -p "$PREFIX/lib" "$PREFIX/include/webp"
  cp "$SLICE_WORK/libwebp/libwebpdecoder.a" "$SLICE_WORK/libwebp/libwebpdemux.a" "$PREFIX/lib/"
  cp "$WT_SRC/libwebp/src/webp/decode.h" \
    "$WT_SRC/libwebp/src/webp/demux.h" \
    "$WT_SRC/libwebp/src/webp/mux_types.h" \
    "$WT_SRC/libwebp/src/webp/types.h" \
    "$PREFIX/include/webp/"
}

# compile_c_lib <name> <output lib> <source dir> <files...>
compile_c_lib() {
  local name="$1" lib="$2" src="$3"
  shift 3
  local dir="$SLICE_WORK/$name" f objs=()
  rm -rf "$dir" && mkdir -p "$dir" "$PREFIX/lib"
  for f in "$@"; do
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -std=gnu99 -I"$src" -c "$src/$f" -o "$dir/${f%.c}.o" \
      >>"$dir/build.log" 2>&1 ||
      { tail -20 "$dir/build.log" >&2; wt_die "$name: compiling $f failed ($SLICE)"; }
    objs+=("$dir/${f%.c}.o")
  done
  rm -f "$PREFIX/lib/$lib"
  "$AR" rcs "$PREFIX/lib/$lib" "${objs[@]}"
}

build_giflib() {
  # Decoder only. HAVE_REALLOCARRAY stays undefined so giflib uses its own
  # overflow-checked reallocarray, which iOS 15 libc lacks.
  compile_c_lib giflib libgif.a "$WT_SRC/giflib" \
    dgif_lib.c gifalloc.c gif_err.c openbsd-reallocarray.c
  mkdir -p "$PREFIX/include"
  cp "$WT_SRC/giflib/gif_lib.h" "$PREFIX/include/"
}

build_lz4() {
  compile_c_lib lz4 liblz4.a "$WT_SRC/lz4/lib" lz4.c lz4hc.c
  mkdir -p "$PREFIX/include"
  cp "$WT_SRC/lz4/lib/lz4.h" "$WT_SRC/lz4/lib/lz4hc.h" "$PREFIX/include/"
}

# The libraries merged into the final archive, per slice.
MERGED_LIBS="libvpx.a libwebm.a libyuv.a libwebpdecoder.a libwebpdemux.a libgif.a liblz4.a"

build_slice() {
  SLICE="$1" ARCH="$2" SDK="$3"
  local triple="$ARCH-apple-ios$IOS_MIN"
  [ "$SDK" = "iphonesimulator" ] && triple="$triple-simulator"

  SLICE_WORK="$IOS_WORK/$SLICE"
  PREFIX="$SLICE_WORK/prefix"
  rm -rf "$SLICE_WORK" && mkdir -p "$PREFIX"

  local sysroot
  sysroot="$(xcrun --sdk "$SDK" --show-sdk-path)"
  CC="$(xcrun --sdk "$SDK" -f clang)"
  CXX="$(xcrun --sdk "$SDK" -f clang++)"
  AR="$(xcrun --sdk "$SDK" -f ar)"
  STRIP="$(xcrun --sdk "$SDK" -f strip)"
  # -fvisibility=hidden keeps the deps' symbols out of the plugin's exported
  # symbol table once they are linked into a framework. libvpx keeps its own
  # -O3, and CMake adds -O3 for Release.
  COMMON_FLAGS="-DNDEBUG -fvisibility=hidden"
  TARGET_FLAGS="-target $triple -isysroot $sysroot"
  # CMake derives target, sysroot and deployment target from CMAKE_OSX_*.
  CMAKE_CFLAGS="$COMMON_FLAGS"
  # Plain compiles for giflib and lz4.
  CFLAGS="$TARGET_FLAGS -O2 $COMMON_FLAGS"

  wt_log "[$SLICE] libvpx"
  build_libvpx
  wt_log "[$SLICE] libwebm"
  build_libwebm
  wt_log "[$SLICE] libyuv"
  build_libyuv
  wt_log "[$SLICE] libwebp"
  build_libwebp
  wt_log "[$SLICE] giflib"
  build_giflib
  wt_log "[$SLICE] lz4"
  build_lz4

  local libs=() l
  for l in $MERGED_LIBS; do
    [ -f "$PREFIX/lib/$l" ] || wt_die "[$SLICE] $l was not produced"
    libs+=("$PREFIX/lib/$l")
  done
  # libtool warns about object files that share a basename across libraries
  # (e.g. two cpu.o); members are looked up by symbol, so that is harmless.
  libtool -static -no_warning_for_no_symbols -o "$SLICE_WORK/lib$FRAMEWORK.a" "${libs[@]}" 2>/dev/null ||
    wt_die "[$SLICE] libtool merge failed"
}

build_slice ios-arm64 arm64 iphoneos
build_slice ios-sim-arm64 arm64 iphonesimulator
build_slice ios-sim-x86_64 x86_64 iphonesimulator

# ---------------------------------------------------------------------------
# Assemble the xcframework.

wt_log "assembling $FRAMEWORK.xcframework"
STAGE="$IOS_WORK/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/device" "$STAGE/simulator"

cp "$IOS_WORK/ios-arm64/lib$FRAMEWORK.a" "$STAGE/device/"
lipo -create \
  "$IOS_WORK/ios-sim-arm64/lib$FRAMEWORK.a" \
  "$IOS_WORK/ios-sim-x86_64/lib$FRAMEWORK.a" \
  -output "$STAGE/simulator/lib$FRAMEWORK.a"

# Headers are identical across slices; drop the CMake/pkg-config leftovers.
for kind in device simulator; do
  cp -R "$IOS_WORK/ios-arm64/prefix/include" "$STAGE/$kind/Headers"
done
if ! diff -r "$IOS_WORK/ios-arm64/prefix/include" "$IOS_WORK/ios-sim-x86_64/prefix/include" >/dev/null; then
  wt_die "public headers differ between device and simulator slices"
fi

mkdir -p "$OUT"
rm -rf "${OUT:?}/$FRAMEWORK.xcframework"
xcodebuild -create-xcframework \
  -library "$STAGE/device/lib$FRAMEWORK.a" -headers "$STAGE/device/Headers" \
  -library "$STAGE/simulator/lib$FRAMEWORK.a" -headers "$STAGE/simulator/Headers" \
  -output "$OUT/$FRAMEWORK.xcframework" >"$IOS_WORK/xcframework.log" 2>&1 ||
  { cat "$IOS_WORK/xcframework.log" >&2; wt_die "xcodebuild -create-xcframework failed"; }

# ---------------------------------------------------------------------------
# Link check: smoke.cc must compile and link against every slice, which catches
# missing objects and unresolved symbols. The binaries are kept so they can be
# run on a simulator: xcrun simctl spawn booted <binary> test_assets

wt_log "link-checking every slice"
XCF="$OUT/$FRAMEWORK.xcframework"
link_check() { # <slice> <triple> <sdk> <xcframework dir>
  local headers="$XCF/$4/Headers"
  "$(xcrun --sdk "$3" -f clang++)" -std=c++17 -Wall -Werror -target "$2" \
    -isysroot "$(xcrun --sdk "$3" --show-sdk-path)" \
    -I"$headers" -I"$headers/webm" \
    "$WT_ROOT/third_party/build/smoke.cc" "$XCF/$4/lib$FRAMEWORK.a" \
    -o "$IOS_WORK/smoke-$1" >"$IOS_WORK/smoke-$1.log" 2>&1 ||
    { cat "$IOS_WORK/smoke-$1.log" >&2; wt_die "smoke test does not link for $1"; }
  # Any linker warning (e.g. a platform mismatch) fails the check too.
  [ ! -s "$IOS_WORK/smoke-$1.log" ] ||
    { cat "$IOS_WORK/smoke-$1.log" >&2; wt_die "smoke test link for $1 printed warnings"; }
}
link_check ios-arm64 "arm64-apple-ios$IOS_MIN" iphoneos ios-arm64
link_check ios-sim-arm64 "arm64-apple-ios$IOS_MIN-simulator" iphonesimulator ios-arm64_x86_64-simulator
link_check ios-sim-x86_64 "x86_64-apple-ios$IOS_MIN-simulator" iphonesimulator ios-arm64_x86_64-simulator

# ---------------------------------------------------------------------------
# Report.

human() { awk -v b="$1" 'BEGIN { printf "%.1f MiB", b / 1048576 }'; }
wt_log "built $OUT/$FRAMEWORK.xcframework"
for lib in "$OUT/$FRAMEWORK.xcframework"/*/lib$FRAMEWORK.a; do
  printf '  %-30s %s\n' "$(basename "$(dirname "$lib")")" "$(lipo -info "$lib" | sed 's/.*: //')"
done
printf '\nPer-slice size (merged static archive, before app linking/dead-stripping):\n'
for slice in ios-arm64 ios-sim-arm64 ios-sim-x86_64; do
  printf '  %-16s %10s bytes  (%s)\n' "$slice" \
    "$(stat -f %z "$IOS_WORK/$slice/lib$FRAMEWORK.a")" \
    "$(human "$(stat -f %z "$IOS_WORK/$slice/lib$FRAMEWORK.a")")"
done
