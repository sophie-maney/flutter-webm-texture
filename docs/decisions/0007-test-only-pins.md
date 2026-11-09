# 0007: Test-only dependencies are pinned in `third_party/test-deps.lock`

- **Status:** Proposed (T1.1)
- **Date:** 2026-11-09

## Context

T1.1 needs GoogleTest for the host test suite. `AGENTS.md` says versions and sha256s live only in `third_party/versions.lock`. But that file also feeds two things GoogleTest must stay out of:
- The `deps-<hash>` release tag (ADR 0003) hashes every row. A GoogleTest row would make a new prebuilt release with identical binaries, and every later test-tool bump would do it again.
- `third_party/build/collect-licenses.sh` requires a build-script mapping and a `LICENSES.md` entry for every row. That file lists what ships in the plugin, and GoogleTest never does.

The other options were `FetchContent` with the hash in CMake (a second place for pins, with its own downloader) or the system GoogleTest from brew/apt (unpinned, and built without our sanitizer flags).

## Decision

- Test-only native dependencies go in `third_party/test-deps.lock`, which uses the `versions.lock` format. Its first entry is GoogleTest 1.18.0.
- They are fetched by the same sha256-checking `wt_fetch_source` in `third_party/build/common.sh`. `tool/build_host_deps.sh` points it at the test lock for these rows.
- Nothing in `test-deps.lock` may be linked into the plugin or the prebuilts.
- `tool/build_host_deps.sh` (host builds of the shipped libraries, plus the test deps) lives in `tool/`, not `third_party/build/`, so editing it doesn't change the release tag either.

## Consequences

- Shipped libraries: `versions.lock`. Test tools: `test-deps.lock`. Both are pinned by sha256, and nothing is fetched unpinned.
- Bumping GoogleTest doesn't touch the prebuilt release.
- `AGENTS.md` ("Native libraries") and `PLAN.md` (repository layout) name both files.
