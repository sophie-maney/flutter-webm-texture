#!/usr/bin/env bash
# Packs a platform's build output into the release archive that
# tool/fetch_prebuilts.dart downloads:
#
#   <out>/webm_texture_deps-android.tar.gz   include/ + <abi>/libwebm_texture_deps.a
#   <out>/webm_texture_deps-ios.tar.gz       WebmTextureDeps.xcframework/
#
# The archive root is the platform directory's contents, so unpacking into
# third_party/prebuilt/<platform> reproduces what build-<platform>.sh wrote.
# The fetch tool refuses archives with absolute or `..` paths and with links;
# keep this layout in step with its `platforms` table.
#
# Usage: third_party/build/package-prebuilts.sh android|ios [--out DIR]
#   --out DIR   where to write the archive (default: third_party/prebuilt/dist)

set -euo pipefail

# shellcheck source=common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[ $# -ge 1 ] || wt_die "usage: package-prebuilts.sh android|ios [--out DIR]"
PLATFORM="$1"
shift
OUT="$WT_ROOT/third_party/prebuilt/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="$2"; shift 2 ;;
    *) wt_die "unknown argument '$1'" ;;
  esac
done
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

SRC="$WT_ROOT/third_party/prebuilt/$PLATFORM"
[ -d "$SRC" ] || wt_die "$SRC does not exist; run build-$PLATFORM.sh first"

case "$PLATFORM" in
  android)
    [ -d "$SRC/include" ] || wt_die "$SRC/include is missing"
    ENTRIES="include"
    for abi_dir in "$SRC"/*/; do
      abi="$(basename "$abi_dir")"
      [ "$abi" = include ] && continue
      [ -f "$abi_dir/libwebm_texture_deps.a" ] || wt_die "$abi has no libwebm_texture_deps.a"
      ENTRIES="$ENTRIES $abi"
    done
    [ "$ENTRIES" != include ] || wt_die "no ABI directories in $SRC"
    ;;
  ios)
    [ -f "$SRC/WebmTextureDeps.xcframework/Info.plist" ] ||
      wt_die "$SRC/WebmTextureDeps.xcframework is missing"
    ENTRIES="WebmTextureDeps.xcframework"
    ;;
  *) wt_die "platform must be android or ios" ;;
esac

mkdir -p "$OUT"
ARCHIVE="$OUT/webm_texture_deps-$PLATFORM.tar.gz"
wt_log "packing $ENTRIES -> $ARCHIVE"
# COPYFILE_DISABLE stops macOS tar adding ._* AppleDouble entries.
# shellcheck disable=SC2086 # ENTRIES is a list of simple names
COPYFILE_DISABLE=1 tar -czf "$ARCHIVE" -C "$SRC" $ENTRIES
ls -l "$ARCHIVE" >&2
