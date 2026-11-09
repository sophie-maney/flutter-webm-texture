#!/usr/bin/env bash
# Spike A (T0.7): builds libvpx, libwebm (with webm_parser) and libyuv for the
# host (macOS or Linux) into build/deps/host/prefix, so the spike CLI can run
# without a device. Throwaway: T1.1 owns the real host build.
#
# Sources come from third_party/versions.lock through common.sh, so they are
# the same sha256-verified tarballs the prebuilts use. libvpx gets the same
# decoder-only flags as the mobile builds, including --size-limit=1024x1024.
#
# Usage: spikes/alpha/build-host-deps.sh [--jobs N]

set -euo pipefail

# shellcheck source=../../third_party/build/common.sh
source "$(dirname "${BASH_SOURCE[0]}")/../../third_party/build/common.sh"

JOBS="$(wt_ncpu)"
while [ $# -gt 0 ]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    *) wt_die "unknown argument '$1'" ;;
  esac
done

wt_require cmake make perl curl git shasum tar
HOST_WORK="$WT_WORK/host"
PREFIX="$HOST_WORK/prefix"
rm -rf "$HOST_WORK" && mkdir -p "$PREFIX"

for lib in libvpx libwebm libyuv; do
  wt_fetch_source "$lib"
done

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
make -C "$HOST_WORK/libvpx" install >>"$HOST_WORK/libvpx.build.log" 2>&1

host_cmake() { # <name> <src> [args...]
  local name="$1" src="$2"
  shift 2
  cmake -S "$src" -B "$HOST_WORK/$name" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@" \
    >"$HOST_WORK/$name.configure.log" 2>&1 ||
    { tail -30 "$HOST_WORK/$name.configure.log" >&2; wt_die "$name configure failed"; }
}

wt_log "[host] libwebm"
host_cmake libwebm "$WT_SRC/libwebm" -DENABLE_WEBM_PARSER=ON -DENABLE_WEBMTS=OFF \
  -DENABLE_WEBMINFO=OFF -DENABLE_TESTS=OFF -DENABLE_SAMPLE_PROGRAMS=OFF
cmake --build "$HOST_WORK/libwebm" -j "$JOBS" --target webm >"$HOST_WORK/libwebm.build.log" 2>&1 ||
  { tail -40 "$HOST_WORK/libwebm.build.log" >&2; wt_die "libwebm build failed"; }
cmake --install "$HOST_WORK/libwebm" >>"$HOST_WORK/libwebm.build.log" 2>&1

wt_log "[host] libyuv"
host_cmake libyuv "$WT_SRC/libyuv" -DLIBYUV_DISABLE_JPEG=ON -DUNIT_TEST=OFF
cmake --build "$HOST_WORK/libyuv" -j "$JOBS" --target yuv >"$HOST_WORK/libyuv.build.log" 2>&1 ||
  { tail -40 "$HOST_WORK/libyuv.build.log" >&2; wt_die "libyuv build failed"; }
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$HOST_WORK/libyuv/libyuv.a" "$PREFIX/lib/"
cp -R "$WT_SRC/libyuv/include/." "$PREFIX/include/"

wt_log "host deps installed in $PREFIX"
ls -l "$PREFIX/lib"/*.a
