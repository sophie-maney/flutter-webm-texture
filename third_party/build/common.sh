# Shared helpers for the third-party build scripts. Source it; don't run it.
#
# Every upstream source is fetched through wt_fetch_source, which checks the
# sha256 pinned in third_party/versions.lock and fails closed on a mismatch.
# Written for the bash 3.2 that ships with macOS.

WT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WT_LOCK="$WT_ROOT/third_party/versions.lock"
WT_WORK="${WT_WORK:-$WT_ROOT/build/deps}"
WT_DOWNLOADS="$WT_WORK/downloads"
WT_SRC="$WT_WORK/src"

# Every library in versions.lock, in build order.
WT_LIBS="libvpx libwebm libyuv libwebp giflib lz4"

wt_log() { printf '\033[1;34m==>\033[0m %s\n' "$*" >&2; }
wt_die() {
  printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
  exit 1
}

wt_require() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null 2>&1 || wt_die "'$tool' is required but not on PATH"
  done
}

# wt_lock_field <name> <version|sha256|url>
wt_lock_field() {
  local col
  case "$2" in
    version) col=2 ;;
    sha256) col=3 ;;
    url) col=4 ;;
    *) wt_die "unknown lock field '$2'" ;;
  esac
  local value
  value="$(awk -v n="$1" -v c="$col" '$1 !~ /^#/ && $1 == n { print $c; found++ }
    END { if (found != 1) exit 1 }' "$WT_LOCK")" ||
    wt_die "versions.lock must have exactly one row for '$1'"
  [ -n "$value" ] || wt_die "versions.lock row for '$1' has no $2"
  printf '%s\n' "$value"
}

wt_sha256() { shasum -a 256 "$1" | awk '{ print $1 }'; }

# wt_verify <file> <expected sha256>; deletes the file and fails on mismatch.
wt_verify() {
  local actual
  actual="$(wt_sha256 "$1")"
  if [ "$actual" != "$2" ]; then
    rm -f "$1"
    wt_die "sha256 mismatch for $(basename "$1"): expected $2, got $actual"
  fi
}

# wt_fetch_source <name>
# Downloads (or reuses a cached copy of) the pinned tarball, verifies it, and
# extracts it into a fresh $WT_SRC/<name>, which it stores in WT_FETCHED.
# Call it directly, not in $(...): bash clears `set -e` in command
# substitutions, so a failure there would go unnoticed.
wt_fetch_source() {
  local name="$1" version sha url tarball dest
  version="$(wt_lock_field "$name" version)" || exit 1
  sha="$(wt_lock_field "$name" sha256)" || exit 1
  url="$(wt_lock_field "$name" url)" || exit 1
  tarball="$WT_DOWNLOADS/$name-$sha.tar"
  dest="$WT_SRC/$name"
  mkdir -p "$WT_DOWNLOADS" "$WT_SRC"

  if [ -f "$tarball" ] && [ "$(wt_sha256 "$tarball")" = "$sha" ]; then
    wt_log "$name $version: using cached download"
  else
    rm -f "$tarball" "$tarball.part"
    case "$url" in
      git+*)
        wt_log "$name $version: fetching ${url#git+}"
        local repo="$WT_DOWNLOADS/$name.git"
        rm -rf "$repo"
        git init -q --bare "$repo"
        git -C "$repo" fetch -q --depth 1 "${url#git+}" "$version" ||
          wt_die "git fetch of $name@$version failed"
        [ "$(git -C "$repo" rev-parse FETCH_HEAD)" = "$version" ] ||
          wt_die "$name: fetched commit is not $version"
        git -C "$repo" archive --format=tar --prefix="$name/" "$version" >"$tarball.part"
        rm -rf "$repo"
        ;;
      https://*)
        wt_log "$name $version: downloading $url"
        curl -fsSL --retry 3 --retry-all-errors -o "$tarball.part" "$url" ||
          wt_die "download of $url failed"
        ;;
      *) wt_die "$name: unsupported url '$url'" ;;
    esac
    wt_verify "$tarball.part" "$sha"
    mv "$tarball.part" "$tarball"
  fi

  # Every upstream tarball has exactly one top-level directory.
  rm -rf "$dest"
  mkdir -p "$dest"
  tar -xf "$tarball" -C "$dest" --strip-components 1 ||
    wt_die "failed to extract $name"
  WT_FETCHED="$dest"
}

# wt_ncpu: parallel job count for make.
wt_ncpu() { sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4; }
