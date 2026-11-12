## 1.0.0 (unreleased)

First release.

* `WebmSticker` widget: plays VP9/VP8 WebM with alpha, animated WebP and GIF
  through platform `Texture`s on Android (API 24+) and iOS (15+). Decoding
  runs in a shared C++ core on native worker threads.
* Sources: `StickerSource.asset`, `.file`, `.bytes` and `.network`, the last
  through an app-provided `StickerFetcher` (`WebmStickers.fetcher`).
* Playback: `autoplay`, `loop`, `WebmStickerController` (`play`, `pause`,
  `seekToFrame`), `placeholder` and `errorBuilder`. Stickers advance only
  while on screen, under an enabled `TickerMode` and in the foreground, and
  widgets showing the same content at the same size share one animation.
* `StickerAtlasScope`: draws many stickers through a few shared atlas
  textures, presented at the content's frame rate.
* Device tiers (`StickerTier`: `auto`, `high`, `mid`, `low`) that follow the
  device class, Low Power Mode and thermal state, with `StickerConfig.maxFps`
  and `maxConcurrent` overrides.
* On-disk frame cache with an LRU budget (`StickerConfig.diskBudgetBytes`,
  200 MiB by default, `cacheDirectory`), `WebmStickers.cacheStats()` and
  `clearCache()`.
* `WebmStickers.probe` reads a sticker's metadata (`StickerInfo`) without
  decoding pixels.
* Hard limits on untrusted input (`StickerLimits`: 2 MiB, 1024 px, 10 s,
  600 frames by default); violations surface as `StickerException`.
