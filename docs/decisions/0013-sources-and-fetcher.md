# 0013: Sticker sources, content keys and the fetcher

- **Status:** Accepted (T2.6)
- **Date:** 2026-11-10

## Context

`PLAN.md` lists four sources and a pluggable `StickerFetcher` set through `WebmStickers.configure(StickerConfig(fetcher: …))`. `StickerConfig` doesn't exist yet (tiers and the disk budget arrive with T3.x), and the content key needs a hash that no dependency in `PLAN.md` provides.

## Decision

- **Sources** are `file`, `bytes`, `asset(name, {bundle, package})` and `network(url, {headers, key})`. They compare by value, so a rebuilt widget with an equal source keeps its animation. `bytes` without a `key` compares by instance.
- **Resolution.** `StickerResolver` turns any source into a `ResolvedSticker` (a file or bytes source plus the key). It loads assets, calls the fetcher, and keeps the last 32 asset and network results, deduplicating concurrent loads. Failures aren't cached. The widget and `WebmStickers.probe` resolve first; the registry and probe only accept resolved sources.
- **Content key.**
  - Caller's `key`: `key:<key>`.
  - Bytes, assets and fetched bytes: `sha1:<hex of the bytes>`, so equal content shares an animation whatever its origin.
  - Files, including a file the fetcher returns: `file:<path>`. Hashing a file would mean reading it on the UI isolate. A fetcher that returns files must give them content-addressed names.
- **SHA-1 in pure Dart** (`lib/src/sha1.dart`), not the `crypto` package, to add no dependency. It names content and isn't a security boundary. It runs once per buffer instance, about 10 ms per MB.
- **`StickerFetcher`** is an interface with `fetch(Uri, {headers})` returning `StickerFetchResult.file(path)` or `.bytes(data)`. The plugin ships no HTTP client. Without a fetcher a network source fails with a `StateError` naming the URL.
- **Registration** is `WebmStickers.fetcher` (a static setter) until `configure` exists; T3.x moves it into `StickerConfig.fetcher`.
- **Errors** from asset loading or the fetcher reach `errorBuilder` unchanged, with no platform call.

## Consequences

- Two network URLs that return the same bytes share one animation, but both are fetched.
- Cached resolutions hold their bytes, at most 32 × `maxBytes`. (T6.1: only enforced since [ADR 0025](0025-security-review-hardening.md), which refuses fetched bytes over 2 MiB; assets are as large as the app ships.)
- A fetcher that overwrites a file in place under the same path keeps serving the old animation. (T6.1: only if it writes a new file and renames it over the old one. Rewriting it in place truncates the file under the core's mapping, which raises SIGBUS.)
