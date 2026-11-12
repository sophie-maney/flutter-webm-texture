#!/usr/bin/env bash
# Generates LICENSE and NOTICE at the repo root from third_party/LICENSES.md
# and third_party/versions.lock, with the plugin's own license text from
# tool/licenses/BSD-3-Clause.txt.
#
# Usage: tool/aggregate_licenses.sh [--check]
#
# Without arguments, rewrites LICENSE and NOTICE. With --check, writes nothing
# and exits non-zero if either file is stale or if a library in versions.lock
# has no license text in LICENSES.md. The output is deterministic (versions.lock
# order, no dates), so CI can run --check.
#
# Library homepages are listed here because LICENSES.md does not carry them.
# Update them when a library's upstream moves.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK="$ROOT/third_party/versions.lock"
MD="$ROOT/third_party/LICENSES.md"
PLUGIN_LICENSE="$ROOT/tool/licenses/BSD-3-Clause.txt"

check=0
case "${1:-}" in
  --check) check=1 ;;
  "") ;;
  *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

for f in "$LOCK" "$MD" "$PLUGIN_LICENSE"; do
  [ -f "$f" ] || { echo "error: missing $f" >&2; exit 2; }
done

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Pass 1 reads versions.lock, pass 2 reads LICENSES.md; the awk program
# validates both and writes the third-party parts of LICENSE and NOTICE.
awk '
BEGIN {
  home["libvpx"]  = "https://github.com/webmproject/libvpx"
  home["libwebm"] = "https://github.com/webmproject/libwebm"
  home["libyuv"]  = "https://chromium.googlesource.com/libyuv/libyuv"
  home["libwebp"] = "https://chromium.googlesource.com/webm/libwebp"
  home["giflib"]  = "https://giflib.sourceforge.net/"
  home["lz4"]     = "https://github.com/lz4/lz4"
  # giflib'\''s COPYING has no copyright line; this is from gif_lib.h.
  copyright_fallback["giflib"] = "Copyright (c) 1997 Eric S. Raymond"
  nb = 0
}

FNR == NR {
  if ($0 ~ /^#/ || NF == 0) next
  lock_n++
  lock_name[lock_n] = $1
  lock_ver[lock_n] = $2
  lock_idx[$1] = lock_n
  next
}

# The closing fence can be glued to the last license line (the libyuv PATENTS
# in LICENSES.md), so a line ending in ``` closes the block too.
in_block {
  text = $0
  closing = (text ~ /```$/)
  if (closing) sub(/```$/, "", text)
  if (!closing || text != "") body = body text "\n"
  if (closing) {
    in_block = 0
    nb++
    blk_lib[nb] = lib
    blk_file[nb] = file
    blk_body[nb] = body
  }
  next
}

/^\| [a-z0-9]+ +\|/ {
  n = split($0, c, "|")
  name = c[2]; gsub(/ /, "", name)
  ver = c[3]; gsub(/[ `]/, "", ver)
  spdx = c[4]; gsub(/ /, "", spdx)
  tver[name] = ver
  tspdx[name] = spdx
  next
}

/^## [a-z0-9]+$/ { lib = substr($0, 4); next }
/^### `/ { file = substr($0, 5); gsub(/`/, "", file); next }
/^```text$/ { in_block = 1; body = ""; next }

END {
  err = 0
  for (i = 1; i <= lock_n; i++) {
    l = lock_name[i]
    if (!(l in tver)) {
      print "error: " l " has no row in third_party/LICENSES.md" > "/dev/stderr"; err = 1
    } else if (tver[l] != lock_ver[i]) {
      print "error: " l " is " lock_ver[i] " in versions.lock but " tver[l] " in LICENSES.md" > "/dev/stderr"; err = 1
    }
    if (!(l in home)) {
      print "error: no homepage for " l " in tool/aggregate_licenses.sh" > "/dev/stderr"; err = 1
    }
    texts = 0
    for (k = 1; k <= nb; k++) {
      if (blk_lib[k] != l || is_patent(blk_file[k])) continue
      if (blk_body[k] ~ /^[ \t\n]*$/) {
        print "error: empty license text for " l " " blk_file[k] > "/dev/stderr"; err = 1
      }
      texts++
    }
    if (texts == 0) {
      print "error: no license text for " l " in third_party/LICENSES.md" > "/dev/stderr"; err = 1
    }
  }
  for (k = 1; k <= nb; k++) {
    if (!(blk_lib[k] in lock_idx)) {
      print "error: LICENSES.md has text for " blk_lib[k] ", which is not in versions.lock" > "/dev/stderr"; err = 1
    }
  }
  if (err) exit 1

  # LICENSE: the third-party texts, without the patent grants (those are in NOTICE).
  lic = tmp "/third_party.txt"
  print "" > lic
  print strrep("=", 80) >> lic
  print "Third-party native libraries" >> lic
  print strrep("=", 80) >> lic
  print "" >> lic
  print "The native libraries bundled in the package'\''s binaries are under the" >> lic
  print "licenses below. NOTICE lists each library'\''s copyright line, upstream" >> lic
  print "and patent grants." >> lic
  for (i = 1; i <= lock_n; i++) {
    l = lock_name[i]
    print "" >> lic
    print strrep("=", 80) >> lic
    print l " " lock_ver[i] " (" tspdx[l] ")" >> lic
    print strrep("=", 80) >> lic
    for (k = 1; k <= nb; k++) {
      if (blk_lib[k] != l || is_patent(blk_file[k])) continue
      print "" >> lic
      print "File: " blk_file[k] >> lic
      print strrep("-", 80) >> lic
      emit(lic, blk_body[k])
    }
  }

  # NOTICE: attribution list, then the patent grants.
  nt = tmp "/notice.txt"
  print "NOTICE" > nt
  print "" >> nt
  print "webm_texture is Copyright 2026 The webm_texture Authors." >> nt
  print "" >> nt
  print "The package'\''s binaries bundle the following third-party libraries. Their" >> nt
  print "full license texts are in LICENSE." >> nt
  for (i = 1; i <= lock_n; i++) {
    l = lock_name[i]
    cp = ""
    for (k = 1; k <= nb && cp == ""; k++) {
      if (blk_lib[k] != l || is_patent(blk_file[k])) continue
      m = split(blk_body[k], lines, "\n")
      for (j = 1; j <= m && cp == ""; j++) if (lines[j] ~ /Copyright/) cp = lines[j]
    }
    if (cp == "") cp = copyright_fallback[l]
    if (cp == "") { print "error: no copyright line for " l " (add a fallback)" > "/dev/stderr"; exit 1 }
    gsub(/^[ \t]+|[ \t]+$/, "", cp)
    print "" >> nt
    print strrep("-", 80) >> nt
    print "Library:   " l >> nt
    print "Version:   " lock_ver[i] >> nt
    print "License:   " tspdx[l] >> nt
    print "Upstream:  " home[l] >> nt
    print "Copyright: " cp >> nt
  }
  print "" >> nt
  print strrep("=", 80) >> nt
  print "Patent grants" >> nt
  print strrep("=", 80) >> nt
  print "" >> nt
  print "These libraries include a patent grant from Google. The full text of each" >> nt
  print "grant follows." >> nt
  for (i = 1; i <= lock_n; i++) {
    l = lock_name[i]
    for (k = 1; k <= nb; k++) {
      if (blk_lib[k] != l || !is_patent(blk_file[k])) continue
      print "" >> nt
      print "File: " l " " blk_file[k] >> nt
      print strrep("-", 80) >> nt
      emit(nt, blk_body[k])
    }
  }
}

function is_patent(path,   base) {
  base = path
  sub(/.*\//, "", base)
  return base ~ /^PATENTS/
}

function strrep(s, n,   out, i) {
  out = ""
  for (i = 0; i < n; i++) out = out s
  return out
}

function emit(file, body) {
  sub(/\n+$/, "", body)
  printf "%s\n", body >> file
}
' tmp="$tmp" "$LOCK" "$MD"

{
  cat "$PLUGIN_LICENSE"
  cat "$tmp/third_party.txt"
} >"$tmp/LICENSE"
cp "$tmp/notice.txt" "$tmp/NOTICE"

if [ "$check" -eq 1 ]; then
  stale=0
  for name in LICENSE NOTICE; do
    if ! cmp -s "$tmp/$name" "$ROOT/$name" 2>/dev/null; then
      echo "error: $name is out of date; run tool/aggregate_licenses.sh" >&2
      stale=1
    fi
  done
  # The diff covers this, but a missing section gets a clearer message.
  for lib in $(awk '$1 !~ /^#/ && NF { print $1 }' "$LOCK"); do
    version="$(awk -v l="$lib" '$1 == l { print $2 }' "$LOCK")"
    if ! awk -v p="$lib $version (" 'index($0, p) == 1 { found = 1 } END { exit !found }' \
      "$ROOT/LICENSE"; then
      echo "error: LICENSE has no section for $lib $version" >&2
      stale=1
    fi
  done
  [ "$stale" -eq 0 ] || exit 1
  echo "LICENSE and NOTICE are up to date."
else
  cp "$tmp/LICENSE" "$ROOT/LICENSE"
  cp "$tmp/NOTICE" "$ROOT/NOTICE"
  echo "Wrote LICENSE and NOTICE."
fi
