# 0003: Release tag covers the build recipe

- **Status:** Proposed (T0.6)
- **Date:** 2026-11-09

## Context

EXECUTION_PLAN.md T0.6 names the prebuilt release `deps-<lockhash>`, the hash of `third_party/versions.lock`. The binaries also depend on the build scripts: changing a libvpx flag such as `--size-limit` (a security limit, see AGENTS.md) produces different binaries from the same lock. With a lock-only tag the old release would keep being fetched, and the workflow would skip the rebuild because the release already exists.

## Decision

The tag is `deps-<first 16 hex of sha256>` over:

1. the `versions.lock` data rows (comments, blank lines and column alignment ignored), then
2. one `sha256  name` line per file directly inside `third_party/build/`, in name order.

`tool/fetch_prebuilts.dart` is the only implementation; the workflow calls it with `--print-tag`. A fresh checkout therefore always fetches binaries built from exactly its own lock and scripts.

## Consequences

- Any edit to a build script, even a comment, yields a new release. That costs one CI run and is harmless.
- A branch that edits the lock or build scripts cannot fetch prebuilts until the workflow has published the matching release on main. PRs build to prove the change, but do not publish.
- `force` on the workflow remains for re-running a release (for example after a flaky upstream), not for picking up changes.
