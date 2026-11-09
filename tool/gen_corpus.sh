#!/usr/bin/env bash
# Generates the synthetic test corpus in test_assets/.
#
#   tool/gen_corpus.sh           Regenerate every clip, reference frame and
#                                manifest.json. Idempotent: on the same machine
#                                and tool versions, a second run is byte-identical.
#   tool/gen_corpus.sh --check   Validate test_assets/manifest.json against the
#                                committed files without re-encoding anything.
#
# Needs: ffmpeg + ffprobe built with libvpx and libopus, img2webp and gif2webp
# (libwebp), gifsicle, jq, gzip and openssl. Every clip is drawn procedurally here; the corpus contains
# no third-party media. See test_assets/README.md for the file formats.
#
# Task: T0.3

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ASSETS="$ROOT/test_assets"
MANIFEST="$ASSETS/manifest.json"
# The committed corpus (clips + refs + manifest) must stay under 5 MB.
MAX_CORPUS_BYTES=5000000

# name|kind|expect. Order here is the order in manifest.json.
CASES="
vp9_alpha_512.webm|webm|ok
vp9_opaque_256.webm|webm|ok
vp8_alpha_256.webm|webm|ok
vp9_alpha_vfr.webm|webm|ok
vp9_odd_301x199.webm|webm|ok
vp9_single_frame.webm|webm|ok
vp9_with_audio.webm|webm|ok
vp9_2048.webm|webm|limit_exceeded
truncated.webm|webm|truncated
garbage.bin|unknown|invalid
anim.webp|webp|ok
anim_disposal.gif|gif|ok
"
TRUNCATE_SOURCE=vp9_alpha_512.webm

die() {
  echo "gen_corpus: $*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "'$1' not found ($2)"
}

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1
  fi
}

fsize() {
  wc -c <"$1" | tr -d ' '
}

check_tools() {
  need ffmpeg "with libvpx and libopus"
  need ffprobe "ships with ffmpeg"
  need jq "JSON processor"
  need gzip "gzip"
  if [ "${1:-}" = gen ]; then
    need img2webp "from libwebp"
    need gif2webp "from libwebp"
    need gifsicle "gifsicle"
    need openssl "openssl"
    local enc
    enc="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
    for e in libvpx-vp9 libvpx libopus gif; do
      grep -q " $e " <<<"$enc" || die "ffmpeg lacks the '$e' encoder"
    done
    local dec
    dec="$(ffmpeg -hide_banner -decoders 2>/dev/null)"
    for d in libvpx-vp9 libvpx webp_anim gif; do
      grep -q " $d " <<<"$dec" || die "ffmpeg lacks the '$d' decoder"
    done
  fi
}

# --- Encoding --------------------------------------------------------------

FF="ffmpeg -nostdin -hide_banner -loglevel error -y"
# Output options that make encoders and muxers deterministic (fixed UIDs, no
# version strings or dates, single-threaded rate control).
DET="-threads 1 -fflags +bitexact -flags +bitexact -map_metadata -1 -map_chapters -1"
# Tag and convert as BT.601 limited range: what VP8 assumes and what the core
# converts with (libyuv I420→RGB).
TO_YUVA="scale=out_color_matrix=bt601:out_range=tv:flags=accurate_rnd+bitexact,format=yuva420p"
TO_YUV="scale=out_color_matrix=bt601:out_range=tv:flags=accurate_rnd+bitexact,format=yuv420p"
COLOR_TAGS="-colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m -color_range tv"
VP9_ALPHA="-c:v libvpx-vp9 -auto-alt-ref 0 -crf 32 -b:v 0 $COLOR_TAGS"

# Smooth RGB gradients that drift over a 3 s period, under an alpha channel
# that holds 0, 255 and every value in between: a soft-edged disc orbiting
# the centre, plus a horizontal alpha ramp along the bottom 15 %.
RGB_EXPR="r='128+127*sin(2*PI*(X/W+T/3))':g='255*Y/H':b='255*(1-X/W)'"
ALPHA_EXPR="a='if(gt(Y,0.85*H),255*X/W,clip(255*(W/4-hypot(X-W/2-W/4*cos(2*PI*T/3),Y-H/2-H/4*sin(2*PI*T/3)))/(W*0.06),0,255))'"

# pattern <w> <h> <fps> [alpha|opaque] → lavfi source
pattern() {
  local a="$ALPHA_EXPR"
  [ "${4:-alpha}" = opaque ] && a="a=255"
  echo "color=c=black@0:s=$1x$2:r=$3,format=rgba,geq=$RGB_EXPR:$a"
}

gen_webm() {
  local out="$1" tmp="$2"

  # 3 s at 30 fps, a second keyframe at frame 45.
  $FF -f lavfi -i "$(pattern 512 512 30),$TO_YUVA" -frames:v 90 \
    $DET $VP9_ALPHA -g 45 "$out/vp9_alpha_512.webm"

  # No alpha. Two-pass with alt-ref, so the stream carries hidden frames
  # inside superframes (60 packets, 65 frames).
  local opaque="-frames:v 60 $DET -c:v libvpx-vp9 -crf 34 -b:v 0 -auto-alt-ref 1 $COLOR_TAGS -passlogfile $tmp/vp9_opaque"
  $FF -f lavfi -i "$(pattern 256 256 30 opaque),$TO_YUV" $opaque -pass 1 -f null -
  $FF -f lavfi -i "$(pattern 256 256 30 opaque),$TO_YUV" $opaque -pass 2 "$out/vp9_opaque_256.webm"

  $FF -f lavfi -i "$(pattern 256 256 30),$TO_YUVA" -frames:v 60 \
    $DET -c:v libvpx -auto-alt-ref 0 -crf 24 -b:v 400k -g 30 $COLOR_TAGS \
    "$out/vp8_alpha_256.webm"

  # Frame durations cycle 40/80/120/200 ms (in a 1 ms timebase).
  local vfr_pts="440*floor(N/4)+40*gte(mod(N,4),1)+80*gte(mod(N,4),2)+120*gte(mod(N,4),3)"
  $FF -f lavfi -i "$(pattern 192 192 30),settb=1/1000,setpts='$vfr_pts',$TO_YUVA" \
    -frames:v 32 $DET -fps_mode passthrough -enc_time_base:v 1/1000 \
    $VP9_ALPHA "$out/vp9_alpha_vfr.webm"

  $FF -f lavfi -i "$(pattern 301 199 30),$TO_YUVA" -frames:v 30 \
    $DET $VP9_ALPHA "$out/vp9_odd_301x199.webm"

  $FF -f lavfi -i "$(pattern 256 256 30),$TO_YUVA" -frames:v 1 \
    $DET $VP9_ALPHA "$out/vp9_single_frame.webm"

  $FF -f lavfi -i "$(pattern 128 128 30),$TO_YUVA" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=1" \
    -map 0:v -map 1:a -frames:v 30 $DET $VP9_ALPHA \
    -c:a libopus -b:a 24k -ac 1 "$out/vp9_with_audio.webm"

  # Over the 1024 px limit; two near-empty frames keep it small.
  $FF -f lavfi -i "color=c=0x3060c0:s=2048x2048:r=30,drawbox=x=896:y=896:w=256:h=256:color=white:t=fill,$TO_YUV" \
    -frames:v 2 $DET -c:v libvpx-vp9 -crf 63 -b:v 0 $COLOR_TAGS "$out/vp9_2048.webm"
}

# Cuts TRUNCATE_SOURCE halfway between the starts of frames k and k+1, where
# k = 2/3 of the way through, so the file ends mid-cluster inside frame k.
gen_truncated() {
  local out="$1" cut
  cut="$(probe "$out/$TRUNCATE_SOURCE" | jq -r "$JQ_LIB"' truncation.cut')"
  head -c "$cut" "$out/$TRUNCATE_SOURCE" >"$out/truncated.webm"
}

gen_garbage() {
  # 4 KiB of deterministic AES-CTR keystream; checked to start with no media magic.
  head -c 4096 /dev/zero |
    openssl enc -aes-128-ctr -nosalt \
      -K 7765626d5f746578747572655f740003 -iv 00000000000000000000000000000000 \
      >"$1/garbage.bin"
  local magic
  magic="$(head -c 4 "$1/garbage.bin" | od -An -tx1 | tr -d ' \n')"
  case "$magic" in
    1a45dfa3 | 52494646 | 47494638) die "garbage.bin starts with a media magic ($magic)" ;;
  esac
}

gen_webp() {
  local out="$1" tmp="$2/webp"
  mkdir -p "$tmp"
  $FF -f lavfi -i "$(pattern 128 128 10)" -frames:v 16 -start_number 0 "$tmp/%02d.png"
  # Frames 0-7 lossless (VP8L), 8-15 lossy with an ALPH chunk; mixed durations.
  (
    cd "$tmp"
    img2webp -loop 0 -lossless \
      -d 100 00.png 01.png 02.png -d 50 03.png 04.png -d 200 05.png -d 100 06.png 07.png \
      -lossy -q 70 \
      -d 100 08.png 09.png -d 250 10.png -d 100 11.png 12.png 13.png -d 50 14.png 15.png \
      -o "$out/anim.webp" >/dev/null
  )
}

# 96x96 canvas, six frames that between them use every GIF disposal method:
#   frame  rect          disposal         content
#   0      0,0 96x96     1 do-not-dispose blue/green checker, transparent hole
#   1      8,8 32x32     2 background     red
#   2      56,8 32x32    3 previous       yellow with a transparent centre
#   3      28,28 40x40   0 unspecified    magenta stripes (interlaced)
#   4      0,72 96x24    2 background     white
#   5      4,76 16x16    1 do-not-dispose cyan
gen_gif() {
  local out="$1" tmp="$2/gif"
  mkdir -p "$tmp"
  sub() { # sub <name> <w> <h> <geq>
    $FF -f lavfi -i "color=c=black@0:s=$2x$3:r=1:d=1,format=rgba,geq=$4" -frames:v 1 \
      -vf "split[a][b];[a]palettegen=reserve_transparent=1:max_colors=8[p];[b][p]paletteuse=dither=none:alpha_threshold=128" \
      $DET "$tmp/$1.gif"
  }
  local cell="mod(floor(X/48)+floor(Y/48),2)"
  sub f0 96 96 "r=0:g='255*$cell':b='255*(1-$cell)':a='255*not(between(X,40,55)*between(Y,40,55))'"
  sub f1 32 32 "r=255:g=0:b=0:a=255"
  sub f2 32 32 "r=255:g=255:b=0:a='255*not(between(X,12,19)*between(Y,12,19))'"
  sub f3 40 40 "r=255:g=0:b=255:a='255*gte(mod(X,8),4)'"
  sub f4 96 24 "r=255:g=255:b=255:a=255"
  sub f5 16 16 "r=0:g=255:b=255:a=255"
  # gifsicle disposal names: none=0, asis=1, background=2, previous=3.
  gifsicle --no-warnings --logical-screen 96x96 --loopcount=forever \
    --disposal asis --delay 10 --position 0,0 "$tmp/f0.gif" \
    --disposal background --delay 20 --position 8,8 "$tmp/f1.gif" \
    --disposal previous --delay 10 --position 56,8 "$tmp/f2.gif" \
    --disposal none --delay 15 --position 28,28 --interlace "$tmp/f3.gif" \
    --disposal background --delay 10 --position 0,72 --no-interlace "$tmp/f4.gif" \
    --disposal asis --delay 30 --position 4,76 "$tmp/f5.gif" \
    -o "$out/anim_disposal.gif"
}

# --- Manifest --------------------------------------------------------------

probe() {
  ffprobe -v error -of json \
    -show_entries stream=index,codec_type,codec_name,width,height:stream_tags=alpha_mode \
    -show_entries packet=stream_index,pts_time,duration_time,flags,pos \
    "$1"
}

# jq helpers over a probe() document.
JQ_LIB='
def ms: tonumber * 1000 | round;
def video: [.streams[] | select(.codec_type == "video")][0];
def codec: video.codec_name | if . == "webp_anim" then "webp" else . end;
def vpackets: (video.index) as $i | [.packets[] | select(.stream_index == $i)];
def pts: [vpackets[].pts_time | ms];
def keyframes: [vpackets | to_entries[] | select(.value.flags | startswith("K")) | .key];
# Matroska only stores start times (DefaultDuration is the nominal frame
# rate), so a frame lasts until the next one starts; the last frame keeps the
# container duration. GIF and WebP store each frame delay explicitly.
def durations($kind):
  if $kind == "webm" then
    pts as $p | vpackets as $v
    | [range(0; $p | length) as $i
       | if $i + 1 < ($p | length) then $p[$i + 1] - $p[$i]
         else ($v[$i].duration_time | ms) end]
  else [vpackets[].duration_time | ms] end;
def truncation:
  vpackets as $v | (($v | length) * 2 / 3 | floor) as $k
  | {complete_frames: $k,
     cut: ((($v[$k].pos | tonumber) + ($v[$k + 1].pos | tonumber)) / 2 | floor)};
'

# entry <file> <kind> <expect> → one manifest.json "files" element
entry() {
  local file="$1" kind="$2" expect="$3" name
  name="$(basename "$file")"
  local base
  base="$(jq -n --arg name "$name" --argjson bytes "$(fsize "$file")" \
    --arg sha256 "$(sha256 "$file")" --arg kind "$kind" --arg expect "$expect" \
    '{name: $name, bytes: $bytes, sha256: $sha256, kind: $kind, expect: $expect}')"

  case "$expect" in
    invalid)
      jq '. + {ref_frames: []}' <<<"$base"
      return
      ;;
    truncated)
      # Describe the frames that survive the cut, using the intact source.
      probe "$(dirname "$file")/$TRUNCATE_SOURCE" |
        jq --argjson base "$base" --arg src "$TRUNCATE_SOURCE" "$JQ_LIB"'
          truncation.complete_frames as $n
          | $base + {
              codec: codec, width: video.width, height: video.height,
              alpha: (video.tags.alpha_mode == "1"), has_audio: false,
              frame_count: $n, pts_ms: pts[:$n], durations_ms: durations("webm")[:$n],
              keyframes: [keyframes[] | select(. < $n)], ref_frames: [],
              truncated_from: $src}'
      return
      ;;
  esac

  probe "$file" | jq --argjson base "$base" --arg kind "$kind" "$JQ_LIB"'
    (pts | length) as $n
    | (if $kind == "webm" then keyframes else [] end) as $kf
    # Reference frames: every frame of the small GIF/WebP canvases; for WebM
    # the first, middle and last frame plus each keyframe and the frame
    # before it (where inter-frame drift is largest).
    | (if $base.expect != "ok" then []
       elif $kind == "webm" then
         [0, ($n / 2 | floor), $n - 1] + $kf + [$kf[] | select(. > 0) | . - 1] | unique
       else [range(0; $n)] end) as $refs
    | $base + {
        codec: codec, width: video.width, height: video.height,
        alpha: (if $kind == "webm" then video.tags.alpha_mode == "1" else true end),
        has_audio: ([.streams[] | select(.codec_type == "audio")] | length > 0),
        frame_count: $n, pts_ms: pts, durations_ms: durations($kind),
        keyframes: $kf, ref_frames: $refs}'
}

# build_manifest <dir> → manifest.json on stdout
build_manifest() {
  local dir="$1" name kind expect
  for line in $CASES; do
    IFS='|' read -r name kind expect <<<"$line"
    [ -f "$dir/$name" ] || die "missing $dir/$name"
    entry "$dir/$name" "$kind" "$expect"
  done | jq -s '{
    version: 1,
    generator: "tool/gen_corpus.sh",
    reference_frames: {
      path: "ref/<name>/<frame, 3 digits>.rgba.gz",
      pixels: "RGBA8888, premultiplied alpha, rows top to bottom, stride = width * 4",
      compression: "gzip",
      color: "YUV decoded as BT.601 limited range"
    },
    files: .
  }'
}

# --- Reference frames ------------------------------------------------------

# Exact premultiply, round(c * a / 255). ffmpeg's premultiply filter rounds
# 255 * 255 down to 254, so it is not used.
PREMULTIPLY="geq=r='round(r(X,Y)*alpha(X,Y)/255)':g='round(g(X,Y)*alpha(X,Y)/255)':b='round(b(X,Y)*alpha(X,Y)/255)':a='alpha(X,Y)',format=rgba"

# extract_refs <dir> <manifest> <tmp>: decode every frame to straight RGBA,
# then premultiply and keep the listed frames.
#   VP8/VP9: libvpx, so the BlockAdditional alpha track is honoured.
#   WebP:    ffmpeg's webp_anim (lossless frames match the source PNGs exactly).
#   GIF:     composited by gif2webp (giflib, browser semantics: the canvas
#            starts transparent and "restore to background" clears to
#            transparent), losslessly, then decoded as WebP. ffmpeg's own GIF
#            decoder fills with the palette background colour instead.
extract_refs() {
  local dir="$1" manifest="$2" tmp="$3"
  jq -r '.files[] | select(.ref_frames | length > 0)
         | "\(.name) \(.codec) \(.width) \(.height) \(.frame_count) \(.ref_frames | join(","))"' \
    "$manifest" |
    while read -r name codec w h n frames; do
      local dec="" src="$dir/$name"
      case "$codec" in
        vp9) dec="-c:v libvpx-vp9" ;;
        vp8) dec="-c:v libvpx" ;;
        gif)
          src="$tmp/$name.webp" # gif2webp is lossless by default
          gif2webp -quiet "$dir/$name" -o "$src"
          ;;
      esac
      local raw="$tmp/$name.rgba" fs=$((w * h * 4))
      $FF $dec -i "$src" -map 0:v:0 -fps_mode passthrough \
        -vf "scale=in_color_matrix=bt601:in_range=tv:flags=accurate_rnd+full_chroma_int+bitexact,format=rgba" \
        -f rawvideo "$raw"
      [ "$(fsize "$raw")" -eq $((fs * n)) ] || die "$name: decoded $(fsize "$raw") bytes, expected $n frames of $fs"
      mkdir -p "$dir/ref/$name"
      local f
      for f in ${frames//,/ }; do
        dd if="$raw" bs="$fs" skip="$f" count=1 2>/dev/null |
          $FF -f rawvideo -pix_fmt rgba -s "${w}x$h" -i - -vf "$PREMULTIPLY" -f rawvideo - |
          gzip -9 -n >"$dir/ref/$name/$(printf %03d "$f").rgba.gz"
      done
      rm -f "$raw"
    done
}

# --- Validation ------------------------------------------------------------

check() {
  [ -f "$MANIFEST" ] || die "no $MANIFEST; run tool/gen_corpus.sh"
  local tmp="$1" fail=0

  # 1. The manifest is exactly what the committed files describe.
  build_manifest "$ASSETS" >"$tmp/manifest.json"
  if ! diff -u "$MANIFEST" "$tmp/manifest.json" >"$tmp/manifest.diff"; then
    echo "manifest.json does not match the files:" >&2
    head -40 "$tmp/manifest.diff" >&2
    fail=1
  fi

  # 2. Every reference frame exists and holds exactly width*height*4 bytes.
  local name w h frames f path got
  while read -r name w h frames; do
    for f in ${frames//,/ }; do
      path="$ASSETS/ref/$name/$(printf %03d "$f").rgba.gz"
      if [ ! -f "$path" ]; then
        echo "missing $path" >&2
        fail=1
        continue
      fi
      got="$(gzip -dc "$path" | wc -c | tr -d ' ')"
      if [ "$got" -ne $((w * h * 4)) ]; then
        echo "$path: $got bytes, expected $((w * h * 4))" >&2
        fail=1
      fi
    done
  done < <(jq -r '.files[] | select(.ref_frames | length > 0)
                  | "\(.name) \(.width) \(.height) \(.ref_frames | join(","))"' "$MANIFEST")

  # 3. Nothing unlisted, and the whole corpus fits the size budget.
  local listed extra
  listed="$(jq -r '.files[] | .name as $n | ($n, (.ref_frames[] | "ref/\($n)/\("00\(.)"[-3:]).rgba.gz"))' "$MANIFEST"
    printf '%s\n' manifest.json README.md)"
  extra="$(cd "$ASSETS" && find . -type f ! -name .DS_Store | sed 's|^\./||' | sort |
    comm -23 - <(sort <<<"$listed"))"
  if [ -n "$extra" ]; then
    echo "unlisted files in test_assets/:" >&2
    echo "$extra" >&2
    fail=1
  fi
  local total
  total="$(cd "$ASSETS" && find . -type f ! -name .DS_Store -exec cat {} + | wc -c | tr -d ' ')"
  if [ "$total" -ge "$MAX_CORPUS_BYTES" ]; then
    echo "corpus is $total bytes, limit $MAX_CORPUS_BYTES" >&2
    fail=1
  fi

  [ "$fail" -eq 0 ] || die "check failed"
  echo "test_assets OK: $(jq '.files | length' "$MANIFEST") files," \
    "$(jq '[.files[].ref_frames | length] | add' "$MANIFEST") reference frames, $total bytes"
}

# --- Main ------------------------------------------------------------------

main() {
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  local tmp="$TMP"

  if [ "${1:-}" = --check ]; then
    check_tools check
    check "$tmp"
    return
  fi
  [ $# -eq 0 ] || die "usage: tool/gen_corpus.sh [--check]"
  check_tools gen

  local out="$tmp/out"
  mkdir -p "$out"
  echo "Encoding clips..."
  gen_webm "$out" "$tmp"
  gen_truncated "$out"
  gen_garbage "$out"
  gen_webp "$out" "$tmp"
  gen_gif "$out" "$tmp"

  echo "Writing manifest and reference frames..."
  build_manifest "$out" >"$out/manifest.json"
  extract_refs "$out" "$out/manifest.json" "$tmp"

  # Replace only what this script owns; README.md is hand-written.
  local name
  for line in $CASES; do
    name="${line%%|*}"
    rm -f "$ASSETS/$name"
    cp "$out/$name" "$ASSETS/$name"
  done
  rm -rf "$ASSETS/ref"
  cp -R "$out/ref" "$ASSETS/ref"
  cp "$out/manifest.json" "$MANIFEST"

  check "$tmp"
}

main "$@"
