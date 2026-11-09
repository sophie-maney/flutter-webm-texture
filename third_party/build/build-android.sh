#!/usr/bin/env bash
# Builds the Android native dependencies as one static archive per ABI:
#
#   <out>/include/                         public headers (identical for every ABI)
#   <out>/<abi>/libwebm_texture_deps.a     every library, merged
#
# ABIs: arm64-v8a, armeabi-v7a (NEON required) and x86_64; x86 is optional
# (--with-x86). Every library in third_party/versions.lock (libvpx, libwebm,
# libyuv, libwebp, giflib, lz4) is merged into the one archive. Sources are
# fetched and sha256-verified by common.sh. Objects are PIC, hidden-visibility
# and built with -ffunction-sections/-fdata-sections, ready to be linked into
# the plugin's .so with --gc-sections and -Wl,-z,max-page-size=16384.
#
# Usage: third_party/build/build-android.sh [options]
#   --out DIR      output directory (default: third_party/prebuilt/android)
#   --ndk DIR      NDK to use (default: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT,
#                  or the newest NDK under $ANDROID_HOME/ndk)
#   --abis LIST    comma-separated ABIs (default: arm64-v8a,armeabi-v7a,x86_64)
#   --with-x86     also build x86
#   --jobs N       parallel compile jobs (default: CPU count)
#
# Needs NDK r28 or newer, cmake, make, perl, and nasm for the x86 ABIs
# (`brew install cmake nasm` / `apt-get install cmake nasm`). Runs on macOS and
# Linux hosts.

set -euo pipefail

# shellcheck source=common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

API=24
NDK_MIN_MAJOR=28
PAGE_SIZE=16384
OUT="$WT_ROOT/third_party/prebuilt/android"
NDK=""
ABIS="arm64-v8a,armeabi-v7a,x86_64"
WITH_X86=0
JOBS="$(wt_ncpu)"
LIB_NAME="webm_texture_deps"

while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    --ndk) NDK="$2"; shift 2 ;;
    --abis) ABIS="$2"; shift 2 ;;
    --with-x86) WITH_X86=1; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    -h | --help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) wt_die "unknown argument '$1' (see --help)" ;;
  esac
done
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
ABIS="$(printf '%s' "$ABIS" | tr ',' ' ')"
if [ "$WITH_X86" = 1 ]; then
  case " $ABIS " in *" x86 "*) ;; *) ABIS="$ABIS x86" ;; esac
fi

# ---------------------------------------------------------------------------
# Toolchain

find_ndk() {
  local cand
  for cand in "$NDK" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK:-}"; do
    [ -n "$cand" ] && { printf '%s\n' "$cand"; return; }
  done
  local sdk
  for sdk in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" "$HOME/Library/Android/sdk" "$HOME/Android/Sdk"; do
    [ -n "$sdk" ] && [ -d "$sdk/ndk" ] || continue
    # Newest side-by-side NDK.
    cand="$(ls "$sdk/ndk" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
    [ -n "$cand" ] && { printf '%s\n' "$sdk/ndk/$cand"; return; }
  done
}

NDK="$(find_ndk)"
[ -n "$NDK" ] || wt_die "no NDK found; pass --ndk or set ANDROID_NDK_HOME"
[ -f "$NDK/source.properties" ] || wt_die "'$NDK' is not an NDK (no source.properties)"
NDK="$(cd "$NDK" && pwd)" # libvpx's configure runs from its build dir
NDK_REVISION="$(awk -F' *= *' '$1 == "Pkg.Revision" { print $2 }' "$NDK/source.properties")"
[ "${NDK_REVISION%%.*}" -ge "$NDK_MIN_MAJOR" ] 2>/dev/null ||
  wt_die "NDK r$NDK_MIN_MAJOR+ is required; $NDK is $NDK_REVISION"

case "$(uname -s)" in
  Darwin) HOST_TAG=darwin-x86_64 ;; # universal binaries on Apple silicon too
  Linux) HOST_TAG=linux-x86_64 ;;
  *) wt_die "unsupported build host $(uname -s); use macOS or Linux" ;;
esac
TC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[ -x "$TC/bin/clang" ] || wt_die "NDK toolchain missing at $TC"
AR="$TC/bin/llvm-ar"
RANLIB="$TC/bin/llvm-ranlib"
STRIP="$TC/bin/llvm-strip"
READELF="$TC/bin/llvm-readelf"

wt_require cmake make perl curl git shasum tar file
for abi in $ABIS; do
  case "$abi" in
    arm64-v8a | armeabi-v7a) ;;
    x86_64 | x86) wt_require nasm ;;
    *) wt_die "unknown ABI '$abi' (arm64-v8a, armeabi-v7a, x86_64, x86)" ;;
  esac
done

wt_log "NDK $NDK_REVISION ($NDK), API $API, ABIs: $ABIS"
ANDROID_WORK="$WT_WORK/android"

# ---------------------------------------------------------------------------
# Sources

for lib in $WT_LIBS; do
  wt_fetch_source "$lib"
done

# ---------------------------------------------------------------------------
# Per-ABI builds. Each ABI installs into $ANDROID_WORK/<abi>/prefix.

build_libvpx() {
  local dir="$ABI_WORK/libvpx"
  rm -rf "$dir" && mkdir -p "$dir"

  # libvpx's arm targets assemble .S files with $AS; clang does that (and
  # configure adds the -c it needs). x86 assembly goes through nasm.
  local as="$CC"
  case "$ABI" in x86_64 | x86) as=nasm ;; esac

  # configure probes nasm's output with a plain `readelf`, which macOS lacks;
  # the NDK's llvm-readelf stands in for it.
  mkdir -p "$dir/bin"
  ln -sf "$READELF" "$dir/bin/readelf"
  local path="$dir/bin:$PATH"

  # EXECUTION_PLAN.md lists --disable-encoders, but libvpx's configure has no
  # such switch; the per-codec --disable-vp{8,9}-encoder flags are the
  # equivalent (ADR 0002).
  # shellcheck disable=SC2086
  (cd "$dir" &&
    PATH="$path" CC="$CC" CXX="$CXX" LD="$CC" AS="$as" AR="$AR" STRIP="$STRIP" \
      "$WT_SRC/libvpx/configure" \
      --target="$VPX_TARGET" \
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
      $VPX_EXTRA) >"$dir/configure.log" 2>&1 ||
    { tail -30 "$dir/configure.log" >&2; wt_die "libvpx configure failed ($ABI)"; }
  PATH="$path" make -C "$dir" -j"$JOBS" >"$dir/build.log" 2>&1 ||
    { tail -40 "$dir/build.log" >&2; wt_die "libvpx build failed ($ABI)"; }
  make -C "$dir" install >>"$dir/build.log" 2>&1 ||
    { tail -20 "$dir/build.log" >&2; wt_die "libvpx install failed ($ABI)"; }
}

# cmake_build <name> <source dir> [cmake args...]
cmake_build() {
  local name="$1" src="$2"
  shift 2
  local dir="$ABI_WORK/$name"
  rm -rf "$dir"
  cmake -S "$src" -B "$dir" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DANDROID_ARM_NEON=ON \
    -DCMAKE_C_FLAGS="$COMMON_FLAGS" \
    -DCMAKE_CXX_FLAGS="$COMMON_FLAGS -fvisibility-inlines-hidden" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF \
    "$@" >"$dir.configure.log" 2>&1 ||
    { tail -30 "$dir.configure.log" >&2; wt_die "$name cmake configure failed ($ABI)"; }
}

cmake_make() {
  local name="$1"
  shift
  cmake --build "$ABI_WORK/$name" -j "$JOBS" "$@" >"$ABI_WORK/$name.build.log" 2>&1 ||
    { tail -40 "$ABI_WORK/$name.build.log" >&2; wt_die "$name build failed ($ABI)"; }
}

build_libwebm() {
  cmake_build libwebm "$WT_SRC/libwebm" \
    -DENABLE_WEBM_PARSER=ON \
    -DENABLE_WEBMTS=OFF \
    -DENABLE_WEBMINFO=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_SAMPLE_PROGRAMS=OFF
  cmake_make libwebm --target webm
  cmake --install "$ABI_WORK/libwebm" >>"$ABI_WORK/libwebm.build.log" 2>&1 ||
    wt_die "libwebm install failed ($ABI)"
}

build_libyuv() {
  cmake_build libyuv "$WT_SRC/libyuv" -DLIBYUV_DISABLE_JPEG=ON -DUNIT_TEST=OFF
  # The `yuv` target is the static library; skip the shared lib and tools.
  cmake_make libyuv --target yuv
  mkdir -p "$PREFIX/lib" "$PREFIX/include"
  cp "$ABI_WORK/libyuv/libyuv.a" "$PREFIX/lib/"
  cp -R "$WT_SRC/libyuv/include/." "$PREFIX/include/"
}

build_libwebp() {
  # Decode-only: libwebpdecoder (no encoder) plus libwebpdemux for
  # WebPAnimDecoder. libwebp's CMake also builds the NDK's cpufeatures, but
  # only an armeabi-v7a build *without* NEON calls it, so it stays out of the
  # bundle (the link check below would catch an unresolved reference).
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
  cp "$ABI_WORK/libwebp/libwebpdecoder.a" "$ABI_WORK/libwebp/libwebpdemux.a" "$PREFIX/lib/"
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
  local dir="$ABI_WORK/$name" f objs=()
  rm -rf "$dir" && mkdir -p "$dir" "$PREFIX/lib"
  for f in "$@"; do
    # shellcheck disable=SC2086
    "$CC" $CFLAGS -std=gnu99 -I"$src" -c "$src/$f" -o "$dir/${f%.c}.o" \
      >>"$dir/build.log" 2>&1 ||
      { tail -20 "$dir/build.log" >&2; wt_die "$name: compiling $f failed ($ABI)"; }
    objs+=("$dir/${f%.c}.o")
  done
  rm -f "$PREFIX/lib/$lib"
  "$AR" rcs "$PREFIX/lib/$lib" "${objs[@]}"
}

build_giflib() {
  # Decoder only. HAVE_REALLOCARRAY stays undefined so giflib uses its own
  # overflow-checked reallocarray, which bionic only has from API 29.
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

# The libraries merged into the final archive, per ABI.
MERGED_LIBS="libvpx.a libwebm.a libyuv.a libwebpdecoder.a libwebpdemux.a libgif.a liblz4.a"

build_abi() {
  ABI="$1"
  local triple
  case "$ABI" in
    arm64-v8a)
      triple=aarch64-linux-android
      VPX_TARGET=arm64-android-gcc
      VPX_EXTRA=""
      ;;
    armeabi-v7a)
      # NEON is required, not detected at runtime: every armeabi-v7a device
      # on API 24+ that we support has it, and the NDK enables it by default.
      triple=armv7a-linux-androideabi
      VPX_TARGET=armv7-android-gcc
      VPX_EXTRA="--enable-neon --disable-runtime-cpu-detect"
      ;;
    x86_64)
      triple=x86_64-linux-android
      VPX_TARGET=x86_64-android-gcc
      VPX_EXTRA="--enable-runtime-cpu-detect"
      ;;
    x86)
      triple=i686-linux-android
      VPX_TARGET=x86-android-gcc
      VPX_EXTRA="--enable-runtime-cpu-detect"
      ;;
  esac

  ABI_WORK="$ANDROID_WORK/$ABI"
  PREFIX="$ABI_WORK/prefix"
  rm -rf "$ABI_WORK" && mkdir -p "$PREFIX"

  # The NDK's <triple><api>-clang wrappers pin target and API level.
  CC="$TC/bin/$triple$API-clang"
  CXX="$TC/bin/$triple$API-clang++"
  [ -x "$CC" ] || wt_die "missing $CC"
  # -fvisibility=hidden keeps the deps' symbols out of the plugin's .so export
  # table; the section flags let its linker --gc-sections unused code.
  COMMON_FLAGS="-DNDEBUG -fPIC -fvisibility=hidden -ffunction-sections -fdata-sections"
  # Plain compiles for giflib and lz4.
  CFLAGS="-O2 $COMMON_FLAGS"

  wt_log "[$ABI] libvpx"
  build_libvpx
  wt_log "[$ABI] libwebm"
  build_libwebm
  wt_log "[$ABI] libyuv"
  build_libyuv
  wt_log "[$ABI] libwebp"
  build_libwebp
  wt_log "[$ABI] giflib"
  build_giflib
  wt_log "[$ABI] lz4"
  build_lz4

  # Merge with an MRI script; llvm-ar keeps members that share a basename
  # (e.g. two cpu.o), and the linker looks them up by symbol.
  local mri="$ABI_WORK/merge.mri" l
  printf 'CREATE %s\n' "$ABI_WORK/lib$LIB_NAME.a" >"$mri"
  for l in $MERGED_LIBS; do
    [ -f "$PREFIX/lib/$l" ] || wt_die "[$ABI] $l was not produced"
    printf 'ADDLIB %s\n' "$PREFIX/lib/$l" >>"$mri"
  done
  printf 'SAVE\nEND\n' >>"$mri"
  rm -f "$ABI_WORK/lib$LIB_NAME.a"
  "$AR" -M <"$mri" || wt_die "[$ABI] archive merge failed"
  # The NDK's CMake toolchain adds -g, so only the CMake-built libraries carry
  # DWARF. Drop it everywhere; symbol tables stay, so crashes still symbolize.
  "$STRIP" --strip-debug "$ABI_WORK/lib$LIB_NAME.a"
  "$RANLIB" "$ABI_WORK/lib$LIB_NAME.a"
}

for abi in $ABIS; do
  build_abi "$abi"
done

# ---------------------------------------------------------------------------
# Install: one shared include/ plus <abi>/lib$LIB_NAME.a.

FIRST_ABI="${ABIS%% *}"
for abi in $ABIS; do
  diff -r "$ANDROID_WORK/$FIRST_ABI/prefix/include" "$ANDROID_WORK/$abi/prefix/include" >/dev/null ||
    wt_die "public headers differ between $FIRST_ABI and $abi"
done

wt_log "installing into $OUT"
mkdir -p "$OUT"
rm -rf "${OUT:?}/include"
cp -R "$ANDROID_WORK/$FIRST_ABI/prefix/include" "$OUT/include"
for abi in $ABIS; do
  rm -rf "${OUT:?}/$abi"
  mkdir -p "$OUT/$abi"
  cp "$ANDROID_WORK/$abi/lib$LIB_NAME.a" "$OUT/$abi/"
done

# ---------------------------------------------------------------------------
# Checks, per ABI:
#  1. Every object in the archive is ELF for the ABI's machine.
#  2. smoke.cc links into a shared library (the plugin's shape) with
#     -z defs (no unresolved symbols), --fatal-warnings, no text relocations,
#     and every LOAD segment aligned to 16 KB.
#  3. smoke.cc also links into an executable, kept so it can be run on a
#     device or emulator:
#       adb push build/deps/android/<abi>/check/smoke test_assets /data/local/tmp/
#       adb shell /data/local/tmp/smoke /data/local/tmp/test_assets

expected_machine() {
  case "$1" in
    arm64-v8a) echo "AArch64" ;;
    armeabi-v7a) echo "ARM" ;;
    x86_64) echo "Advanced Micro Devices X86-64" ;;
    x86) echo "Intel 80386" ;;
  esac
}

check_abi() {
  local abi="$1" triple lib="$OUT/$1/lib$LIB_NAME.a" dir="$ANDROID_WORK/$1/check"
  rm -rf "$dir" && mkdir -p "$dir"
  case "$abi" in
    arm64-v8a) triple=aarch64-linux-android ;;
    armeabi-v7a) triple=armv7a-linux-androideabi ;;
    x86_64) triple=x86_64-linux-android ;;
    x86) triple=i686-linux-android ;;
  esac

  local machines want
  machines="$("$READELF" -h "$lib" | awk -F': *' '/^ *Machine:/ { print $2 }' | sort -u)"
  want="$(expected_machine "$abi")"
  [ "$machines" = "$want" ] ||
    wt_die "[$abi] archive holds objects for '$machines', expected '$want'"

  local cxx="$TC/bin/$triple$API-clang++"
  local flags=(-std=c++17 -Wall -Werror -I"$OUT/include" -I"$OUT/include/webm"
    -ffunction-sections -fdata-sections -static-libstdc++
    -Wl,-z,max-page-size=$PAGE_SIZE -Wl,--gc-sections -Wl,--fatal-warnings)
  "$cxx" "${flags[@]}" -fPIC -shared -Wl,-z,defs \
    "$WT_ROOT/third_party/build/smoke.cc" "$lib" -o "$dir/libsmoke.so" \
    >"$dir/link.log" 2>&1 ||
    { cat "$dir/link.log" >&2; wt_die "[$abi] smoke.cc does not link into a shared library"; }
  "$cxx" "${flags[@]}" -fPIE -pie \
    "$WT_ROOT/third_party/build/smoke.cc" "$lib" -o "$dir/smoke" \
    >>"$dir/link.log" 2>&1 ||
    { cat "$dir/link.log" >&2; wt_die "[$abi] smoke.cc does not link into an executable"; }
  [ ! -s "$dir/link.log" ] ||
    { cat "$dir/link.log" >&2; wt_die "[$abi] smoke link printed warnings"; }

  local bin align
  for bin in "$dir/libsmoke.so" "$dir/smoke"; do
    align="$("$READELF" -lW "$bin" | awk '$1 == "LOAD" { print $NF }' | sort -u)"
    [ "$align" = "0x4000" ] ||
      wt_die "[$abi] $(basename "$bin") LOAD alignment is '$align', expected 0x4000"
    ! "$READELF" -dW "$bin" | grep -q TEXTREL ||
      wt_die "[$abi] $(basename "$bin") has text relocations"
  done

  "$STRIP" --strip-unneeded -o "$dir/libsmoke.stripped.so" "$dir/libsmoke.so"
}

wt_log "checking every ABI"
for abi in $ABIS; do
  check_abi "$abi"
done

# ---------------------------------------------------------------------------
# Report.

size_of() { wc -c <"$1" | tr -d ' '; }
human() { awk -v b="$1" 'BEGIN { printf "%.2f MiB", b / 1048576 }'; }

wt_log "built $OUT"
printf '\n%-12s %-32s %16s %16s\n' "ABI" "machine" "archive" "linked smoke .so"
for abi in $ABIS; do
  a="$(size_of "$OUT/$abi/lib$LIB_NAME.a")"
  s="$(size_of "$ANDROID_WORK/$abi/check/libsmoke.stripped.so")"
  printf '%-12s %-32s %16s %16s\n' "$abi" "$(expected_machine "$abi")" "$(human "$a")" "$(human "$s")"
done
printf '\narchive: merged static archive (debug info stripped, before linking).\n'
printf 'linked smoke .so: smoke.cc + the deps it reaches + static libc++,\n'
printf 'linked with --gc-sections and stripped; LOAD segments 16 KB aligned.\n\n'
for abi in $ABIS; do
  printf '%s: %s\n' "$abi" "$(file -b "$ANDROID_WORK/$abi/check/libsmoke.stripped.so")"
done
