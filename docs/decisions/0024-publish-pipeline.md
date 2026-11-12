# 0024: Publish pipeline

- **Status:** Accepted (T6.4)
- **Date:** 2026-11-12

## Context

`third_party/prebuilt/` is gitignored and pub honours `.gitignore`, so a plain `pub publish` would ship a package with no native libraries. The iOS package also reaches `src/` and the xcframework through symlinks (`ios/webm_texture/Sources/wt_core/*`, `Frameworks/`), and `example/test_assets` is a symlink to the root corpus.

## Decision

1. **`.pubignore` at the root.** It replaces `.gitignore` for pub, so it repeats the root `.gitignore` rules except `/third_party/prebuilt/`, and adds what consumers don't need: `.github/`, `native_test/`, `fuzz/`, `spikes/`, `test_assets/`, `docs/` (spike reports, ADRs; pub warns about the plural name and pub.dev resolves README links against the repository), `third_party/build/` and `test-deps.lock` (`LICENSES.md` and `versions.lock` stay), `tool/`, `test/`, `pigeons/`, `ffigen.yaml`, the plan files, `AGENTS.md`, `example/spike_*`, `example/integration_test`, `example/test`, `example/test_assets`. It keeps `src/`, `android/`, `ios/`, `lib/`, `example/lib` plus the example's platform folders, and the prebuilts.
2. **Symlinks need no staging copy.** `dart pub publish` follows both file and directory symlinks and stores their targets as regular files, so the archive has real `ios/webm_texture/Sources/wt_core/**` and `Frameworks/WebmTextureDeps.xcframework`. The repository layout is unchanged. `/third_party/prebuilt/ios/` is excluded from the archive so the xcframework isn't shipped twice. (`example/test_assets` is excluded, so the archived example can't load its demo assets; it still builds if the assets are supplied. Shipping an example that runs standalone is a T6.6 decision.)
3. **`tool/vendor_prebuilts.dart`.** `vendor` runs `fetch_prebuilts.dart` (sha256 against `SHA256SUMS`, fail closed), then checks completeness, that every symlink resolves, that any shipped `.so` has 16 KB LOAD alignment (none is shipped: the prebuilts are static archives and Gradle builds `libwebm_texture.so`), and a 40 MiB ceiling per platform (14 and 12 MiB today). `archive` runs `flutter pub publish --dry-run`, fails on any warning, stages the files it lists with symlinks resolved, writes a `.tar.gz` and fails above 40 MiB compressed (7.5 MiB today; pub.dev's limit is 100 MiB). pub has no `--to-archive`, so the list is read from the dry run's tree.
4. **`meta` becomes a dependency.** Pigeon's generated `messages.g.dart` imports `package:meta`, which pub requires in `dependencies`. `meta` is pinned by the Flutter SDK, so this adds no new package to the resolution.
5. **`release.yml`** runs on `v*` tags only: tag/version/CHANGELOG check → fvm → vendor → dry run + archive → pana (pinned 0.23.19, must score 160/160) → artifacts (archive, `pana.json`, `pana.txt`). The score threshold is the maximum because the release is cut from the default branch, where pana's repository check passes; lowering it needs an ADR.
6. **Real publish: pub.dev OIDC automated publishing, off by default.** The `publish` job runs only when the repository variable `PUBLISH_TO_PUB_DEV` is `true`, in the `pub-dev` environment (required reviewers), on the original repository, and it publishes the archive that passed (sha256 re-checked). No token is stored. We chose OIDC over manual publishing so the published bytes are exactly the CI-checked bytes.

## One-time setup (human, at T6.6)

- pub.dev: the package must exist first, so publish `1.0.0` once by hand (`flutter pub publish` from the unpacked archive in the release artifact, with a personal login). Then Admin → Automated publishing → GitHub Actions: repository `sophie-maney/flutter-webm-texture`, tag pattern `v{{version}}`, **Require GitHub Actions environment**: `pub-dev`.
- GitHub: Settings → Environments → create `pub-dev`, add required reviewers, restrict deployment to tags `v*`. Then Settings → Variables → add `PUBLISH_TO_PUB_DEV=true` only when ready.
- The publish job is untested until then (it needs the pub.dev side); the build job, dry run and pana are exercised locally.

## Follow-ups for T6.6

- The package name, the Android namespace/group `com.example.webm_texture` and the `CHANGELOG` heading `(unreleased)` (the workflow rejects a tag while it says so).
- pana's repository check reads `pubspec.yaml` from the default branch, so it docks 10 points until this lands there.
