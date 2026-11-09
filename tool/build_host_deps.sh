#!/usr/bin/env bash
# Builds the native deps for the host (macOS or Linux) so the core and its
# GoogleTest suite can build and run without a device (T1.1).
#
# Output: build/deps/host-core/prefix
#   include/                  libvpx, libyuv, libwebp, giflib and lz4 headers
#   lib/                      static libraries, release builds, no sanitizers
#   src/googletest/           GoogleTest sources; the root CMakeLists.txt
#                             compiles them with the same flags as the tests
#
# Sources come through third_party/build/common.sh, so they are the same
# sha256-verified tarballs as the prebuilts: libraries from versions.lock,
# GoogleTest from test-deps.lock (ADR 0007). libvpx gets the mobile decoder-only
# flags, including --size-limit=1024x1024. libwebm is left out (ADR 0004).
#
# This script lives in tool/ rather than third_party/build/ so editing it
# doesn't change the prebuilt release tag (ADR 0003).
#
# Usage: tool/build_host_deps.sh [--jobs N]
# Needs cmake, make, a C/C++ compiler, perl, curl, shasum; nasm on x86_64.

set -euo pipefail

# shellcheck source=../third_party/build/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/../third_party/build/common.sh"

JOBS="$(wt_ncpu)"
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    -h | --help) sed -n '2,22p' "$0"; exit 0 ;;
    *) wt_die "unknown argument '$1'" ;;
  esac
done

wt_require cmake make perl curl shasum tar cc ar
case "$(uname -m)" in
  x86_64 | amd64) wt_require nasm ;;
esac

HOST_WORK="$WT_WORK/host-core"
PREFIX="$HOST_WORK/prefix"
rm -rf "$HOST_WORK"
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$PREFIX/src"

for lib in libvpx libyuv libwebp giflib lz4; do
  wt_fetch_source "$lib"
done
WT_LOCK="$WT_ROOT/third_party/test-deps.lock" wt_fetch_source googletest
cp -R "$WT_FETCHED" "$PREFIX/src/googletest"

# libvpx's configure only knows darwin kernels up to 25, so name the target on
# macOS. On Linux its own detection works.
vpx_target=""
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) vpx_target="--target=arm64-darwin23-gcc" ;;
  Darwin-x86_64) vpx_target="--target=x86_64-darwin23-gcc --as=nasm" ;;
esac

wt_log "[host] libvpx"
mkdir -p "$HOST_WORK/libvpx"
# shellcheck disable=SC2086
(cd "$HOST_WORK/libvpx" &&
  "$WT_SRC/libvpx/configure" $vpx_target \
    --prefix="$PREFIX" \
    --disable-vp8-encoder --disable-vp9-encoder \
    --enable-vp8-decoder --enable-vp9-decoder \
    --disable-vp9-highbitdepth --size-limit=1024x1024 --enable-pic \
    --disable-examples --disable-tools --disable-docs --disable-unit-tests \
    --enable-static --disable-shared --disable-libyuv --disable-webm-io \
    --disable-install-bins --disable-install-srcs) >"$HOST_WORK/libvpx.configure.log" 2>&1 ||
  { tail -30 "$HOST_WORK/libvpx.configure.log" >&2; wt_die "libvpx configure failed"; }
make -C "$HOST_WORK/libvpx" -j"$JOBS" >"$HOST_WORK/libvpx.build.log" 2>&1 ||
  { tail -40 "$HOST_WORK/libvpx.build.log" >&2; wt_die "libvpx build failed"; }
make -C "$HOST_WORK/libvpx" install >>"$HOST_WORK/libvpx.build.log" 2>&1 ||
  { tail -20 "$HOST_WORK/libvpx.build.log" >&2; wt_die "libvpx install failed"; }

host_cmake() { # <name> <src> <targets> [cmake args...]
  local name="$1" src="$2" targets="$3"
  shift 3
  cmake -S "$src" -B "$HOST_WORK/$name" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" \
    >"$HOST_WORK/$name.configure.log" 2>&1 ||
    { tail -30 "$HOST_WORK/$name.configure.log" >&2; wt_die "$name configure failed"; }
  # shellcheck disable=SC2086
  cmake --build "$HOST_WORK/$name" -j "$JOBS" --target $targets \
    >"$HOST_WORK/$name.build.log" 2>&1 ||
    { tail -40 "$HOST_WORK/$name.build.log" >&2; wt_die "$name build failed"; }
}

wt_log "[host] libyuv"
host_cmake libyuv "$WT_SRC/libyuv" yuv -DLIBYUV_DISABLE_JPEG=ON -DUNIT_TEST=OFF
cp "$HOST_WORK/libyuv/libyuv.a" "$PREFIX/lib/"
cp -R "$WT_SRC/libyuv/include/." "$PREFIX/include/"

wt_log "[host] libwebp"
# Decode-only, as on mobile: libwebpdecoder plus libwebpdemux.
host_cmake libwebp "$WT_SRC/libwebp" "webpdecoder webpdemux" \
  -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
  -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF \
  -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_LIBWEBPMUX=OFF -DWEBP_BUILD_WEBPMUX=OFF \
  -DWEBP_BUILD_EXTRAS=OFF
cp "$HOST_WORK/libwebp/libwebpdecoder.a" "$HOST_WORK/libwebp/libwebpdemux.a" "$PREFIX/lib/"
mkdir -p "$PREFIX/include/webp"
for h in decode.h demux.h mux_types.h types.h; do
  cp "$WT_SRC/libwebp/src/webp/$h" "$PREFIX/include/webp/"
done

compile_c_lib() { # <name> <output lib> <source dir> <files...>
  local name="$1" lib="$2" src="$3"
  shift 3
  local dir="$HOST_WORK/$name" f objs=()
  mkdir -p "$dir"
  for f in "$@"; do
    cc -O2 -DNDEBUG -fPIC -std=gnu99 -I"$src" -c "$src/$f" -o "$dir/${f%.c}.o" \
      >>"$dir/build.log" 2>&1 ||
      { tail -20 "$dir/build.log" >&2; wt_die "$name: compiling $f failed"; }
    objs+=("$dir/${f%.c}.o")
  done
  ar rcs "$PREFIX/lib/$lib" "${objs[@]}"
}

wt_log "[host] giflib"
compile_c_lib giflib libgif.a "$WT_SRC/giflib" \
  dgif_lib.c gifalloc.c gif_err.c openbsd-reallocarray.c
cp "$WT_SRC/giflib/gif_lib.h" "$PREFIX/include/"

wt_log "[host] lz4"
compile_c_lib lz4 liblz4.a "$WT_SRC/lz4/lib" lz4.c lz4hc.c
cp "$WT_SRC/lz4/lib/lz4.h" "$WT_SRC/lz4/lib/lz4hc.h" "$PREFIX/include/"

# libvpx also installs a pkgconfig dir we don't use.
rm -rf "$PREFIX/lib/pkgconfig"
wt_log "host deps installed in $PREFIX"
ls -l "$PREFIX/lib"
