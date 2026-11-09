#!/usr/bin/env bash
# Spike A (T0.7): checks EVERY frame (the corpus refs are sampled, ADR 0001)
# of each corpus WebM, plus extra encoder/muxer variants the corpus lacks,
# against ffmpeg's libvpx decode. Variants go to build/spike-alpha/extra/.
#
# Needs ffmpeg with libvpx and a built build/spike-alpha/alpha_spike.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$ROOT/build/spike-alpha/alpha_spike"
EXTRA="$ROOT/build/spike-alpha/extra"
A="$ROOT/test_assets"
rm -rf "$EXTRA" && mkdir -p "$EXTRA"
FF="ffmpeg -nostdin -hide_banner -loglevel error -y"

# Source: the 512 px alpha clip decoded with libvpx (keeps alpha).
SRC=(-c:v libvpx-vp9 -i "$A/vp9_alpha_512.webm")

# VP9 + alpha with alt-ref, so both streams carry hidden frames in
# superframes (8 of 90 packets each). libvpx-vp9 only uses alt-ref in 2-pass.
ALTREF=(-c:v libvpx-vp9 -pix_fmt yuva420p -crf 32 -b:v 0 -auto-alt-ref 1
  -lag-in-frames 16 -passlogfile "$EXTRA/altref")
$FF "${SRC[@]}" "${ALTREF[@]}" -pass 1 -f null - 2>/dev/null
$FF "${SRC[@]}" "${ALTREF[@]}" -pass 2 "$EXTRA/vp9_alpha_altref.webm"
# (VP8 + alpha + alt-ref is refused by ffmpeg: "Transparency encoding with
# auto_alt_ref does not work", so there is no VP8 variant.)
# Tagged BT.709: libvpx writes the colour space into the VP9 bitstream.
$FF "${SRC[@]}" -c:v libvpx-vp9 -pix_fmt yuva420p -crf 32 -b:v 0 \
  -colorspace bt709 -color_primaries bt709 -color_trc bt709 "$EXTRA/vp9_alpha_bt709.webm"
# Live-style remux through a pipe (unknown sizes, no Cues), stream copy.
$FF -i "$A/vp9_alpha_512.webm" -c copy -f webm -live 1 - >"$EXTRA/vp9_alpha_live.webm"

dump() { # <webm> <out.rgba> [color matrix]
  local codec dec
  codec="$(ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$1")"
  dec="libvpx-vp9"
  [ "$codec" = vp8 ] && dec="libvpx"
  $FF -c:v "$dec" -i "$1" -map 0:v:0 -fps_mode passthrough \
    -vf "scale=in_color_matrix=${3:-bt601}:in_range=tv:flags=accurate_rnd+full_chroma_int+bitexact,format=rgba" \
    -f rawvideo "$2"
}

echo "| file | codec | size | AlphaMode | frames (ours/ffmpeg) | with alpha | alpha exact | alpha max err | min alpha PSNR | min RGB PSNR | demuxers agree |"
echo "|---|---|---|---|---|---|---|---|---|---|---|"
status=0
for f in "$A"/vp9_alpha_512.webm "$A"/vp9_opaque_256.webm "$A"/vp8_alpha_256.webm \
  "$A"/vp9_alpha_vfr.webm "$A"/vp9_odd_301x199.webm "$A"/vp9_single_frame.webm \
  "$A"/vp9_with_audio.webm "$EXTRA"/*.webm; do
  matrix=bt601
  case "$f" in *bt709*) matrix=auto ;; esac  # ffmpeg honours the stream tag
  dump "$f" "$EXTRA/$(basename "$f").rgba" "$matrix"
  "$BIN" compare "$f" "$EXTRA/$(basename "$f").rgba" || status=1
done
exit $status
