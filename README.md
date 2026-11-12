<img src="assets/icon.svg" width="64" height="64" alt="">

# flutter-webm-texture

Animated stickers in Flutter, the way Telegram plays them: VP9/VP8 WebM with
alpha, animated WebP and GIF. A shared C++ core decodes in software, caches
the decoded frames on disk, and draws them into Flutter `Texture`s, so a
screen full of stickers stays smooth and cheap.

## Features

- **Transparent video stickers**: WebM (VP9 and VP8, with alpha), animated
  WebP and GIF behind one widget, `WebmSticker`.
- **Off the UI thread**: decoding runs on native worker threads, never on the
  platform main thread or the Flutter UI and raster threads.
- **Plays only what you see**: a sticker advances only while it is on screen,
  under an enabled `TickerMode`, and the app is in the foreground.
- **Shared animations**: widgets showing the same content at the same size
  share one decode.
- **Atlas mode**: `StickerAtlasScope` draws a whole grid through a few shared
  textures instead of one texture per sticker.
- **Frame cache**: decoded frames are kept in a bounded on-disk cache, so
  replays and later launches skip decoding.
- **Device tiers**: frame rate and concurrency follow the device's class, Low
  Power Mode and thermal state, with overrides.
- **Hostile-input safe**: every input is checked against hard limits.

## Formats and platforms

| Format | Codec | Alpha |
|---|---|---|
| WebM | VP9, VP8 | yes (audio tracks are ignored) |
| WebP | animated WebP | yes |
| GIF | GIF | 1-bit transparency |

Android and iOS only: at least Android 7.0 (API 24) and iOS 15.0. There is
no web or desktop support, and no encoding, audio or Lottie/TGS support.

## Limits

Sticker files come from other users, so every input is treated as hostile.
The core rejects anything over these defaults with a `WT_ERR_LIMIT`
`StickerException` (reaching your `errorBuilder`), and malformed input
becomes an error, never a crash:

| Limit | Default |
|---|---|
| `StickerLimits.maxBytes` | 2 MiB |
| `StickerLimits.maxDimension` (width or height) | 1024 px |
| `StickerLimits.maxDuration` | 10 s |
| `StickerLimits.maxFrames` | 600 |

The core also caps the decode work of one pass at 160 Mi pixels (width ×
height summed over all frames: 600 frames at 512 × 512, plus margin). The
bounds keep one sticker from exhausting memory or CPU. The dimension limit
matches libvpx's own size limit.

The widget always applies the defaults. `StickerLimits` is accepted by
`WebmStickers.probe`, which can check a file against tighter or looser
limits before you show it.

## Quick start

```yaml
dependencies:
  webm_texture: ^1.0.0
```

```dart
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

class Hello extends StatelessWidget {
  const Hello({super.key});

  @override
  Widget build(BuildContext context) {
    return const WebmSticker(
      source: StickerSource.asset('assets/wave.webm'),
      width: 160,
      height: 160,
      placeholder: SizedBox.expand(),
    );
  }
}
```

Declare the file as an asset in `pubspec.yaml`, as for any other asset.

A `StickerSource` is one of:

- `StickerSource.asset(name, {bundle, package})`: a bundled asset.
- `StickerSource.file(path)`: an absolute path on disk.
- `StickerSource.bytes(data, {key})`: encoded bytes in memory. Pass a `key`
  to name the content cheaply; without it the SHA-1 of the bytes is used.
- `StickerSource.network(url, {headers, key})`: needs a `StickerFetcher`
  (below).

`WebmSticker` takes `width`, `height`, `fit` (default `BoxFit.contain`),
`autoplay` (default `true`), `loop` (default `true`), `controller`,
`placeholder` (shown until the first frame) and `errorBuilder` (shown when
the sticker can't be played).

Control playback with a `WebmStickerController`:

```dart
final controller = WebmStickerController(); // playing

WebmSticker(
  source: const StickerSource.asset('assets/wave.webm'),
  controller: controller,
  loop: false,
  errorBuilder: (context, error) => const Icon(Icons.broken_image),
);

controller.pause();
controller.seekToFrame(0);
controller.play();
```

A sticker with a controller (or with `loop: false`, or `autoplay: false`)
gets an animation of its own, so pausing or seeking one never affects a copy
elsewhere. A non-looping sticker stops on its last frame; `seekToFrame`
moves it back.

To read a file's metadata without decoding pixels:

```dart
final info = await WebmStickers.probe(
  const StickerSource.asset('assets/wave.webm'),
);
debugPrint('${info.width}x${info.height}, ${info.frameCount} frames');
```

## Atlas mode

For grids and pickers, wrap the stickers in a `StickerAtlasScope`. Below it,
every `WebmSticker` is drawn into a few shared atlas textures instead of a
texture each, which keeps 60+ visible stickers cheap:

```dart
StickerAtlasScope(
  child: GridView.builder(
    gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
      crossAxisCount: 4,
    ),
    itemCount: sources.length,
    itemBuilder: (context, i) => WebmSticker(source: sources[i]),
  ),
)
```

- A sticker holds its slot only while some of it is on screen. Scrolled
  away, its animation is released after a short grace period.
- Pages are 2048 px wide. `maxPages` (default 4, 1 to 16) and
  `maxPageHeight` (default 2048 physical px, 16 to 2048) bound the graphics
  memory: a page costs width × height × 4 bytes per buffer, with 2 (iOS) to
  7 (Android) buffers per page.
- When every page is full, further stickers play standalone with a texture
  each.
- Pages are presented at the content's rate (30 fps for typical stickers)
  rather than the display's.

## Network sources

The plugin ships no HTTP client. Implement a `StickerFetcher` over your own
(with your own caching, auth and retries) and register it once at startup:

```dart
import 'dart:io';
import 'dart:typed_data';

import 'package:webm_texture/webm_texture.dart';

class HttpFetcher implements StickerFetcher {
  static const _maxBytes = 2 << 20;

  @override
  Future<StickerFetchResult> fetch(
    Uri url, {
    Map<String, String>? headers,
  }) async {
    final client = HttpClient();
    try {
      final request = await client.getUrl(url);
      headers?.forEach(request.headers.set);
      final response = await request.close();
      if (response.statusCode != 200) {
        throw HttpException('HTTP ${response.statusCode}', uri: url);
      }
      final builder = BytesBuilder(copy: false);
      await for (final chunk in response) {
        builder.add(chunk);
        // Refuse unreasonably large downloads early.
        if (builder.length > _maxBytes) {
          throw HttpException('Sticker too large', uri: url);
        }
      }
      return StickerFetchResult.bytes(builder.takeBytes());
    } finally {
      client.close();
    }
  }
}

void main() {
  WebmStickers.fetcher = HttpFetcher();
  // runApp(...)
}
```

Then `WebmSticker(source: StickerSource.network('https://example.com/a.webm'))`.

The fetcher is called on the UI isolate, at most once at a time per distinct
source. Throw to report a failure; the exception reaches the widget's
`errorBuilder`. Return `StickerFetchResult.file(path)` instead when you
already keep downloads on disk, and name those files after their content (a
hash, or a version in the path), because the path identifies the sticker in
the frame cache. Without a fetcher, network stickers fail with a
`StateError`.

## Device tiers

How much work stickers may do depends on a `StickerTier`:

| Tier | Frame rate | Concurrent stickers |
|---|---|---|
| `high` | source rate, at most 60 fps | no limit |
| `mid` | at most 30 fps | 30 |
| `low` | at most 15 fps | 8; the others show their first frame |
| `auto` (default) | detected from the device | detected |

With `auto`, the plugin picks a tier from the device's memory, cores and
performance class, and lowers it by one for Low Power Mode (battery saver on
Android) and by one or two for serious or critical thermal pressure, but not
below `low`. When the device's state changes, running stickers follow at
once. Any other tier is used as given.

Override it with a `StickerConfig`, once at startup, before the first sticker
is shown:

```dart
await WebmStickers.configure(
  const StickerConfig(
    tier: StickerTier.mid, // skip detection
    maxFps: 24, // 1..240, overrides the tier's cap
    maxConcurrent: 12, // overrides the tier's limit; 0 means no limit
  ),
);
```

`WebmStickers.debugTier` and `WebmStickers.debugActiveCount` are
`ValueListenable`s for debug overlays and tests.

## Frame cache

Decoded frames are written to one `.wsc` file per animation and size, so a
sticker that has played through once replays, and starts on later launches,
without decoding.

- **Location**: `webm_texture` under the app's cache directory, or
  `StickerConfig.cacheDirectory` (an absolute path). The plugin only reads,
  writes and deletes the `*.wsc` files it made there.
- **Budget**: `StickerConfig.diskBudgetBytes`, default 200 MiB. The least
  recently used files are deleted to stay within it, at startup and after
  each write. Zero keeps nothing on disk.
- **Inspect and clear**:

```dart
final stats = await WebmStickers.cacheStats(); // fileCount, totalBytes
await WebmStickers.clearCache(); // playing stickers carry on
```

`configure` applies the budget at once. Animations created before it keep
the files they use.

## Threading and performance

- All decoding happens on native worker threads (half the cores, 1 to 4, and
  fewer at the lower tiers). `probe`, `cacheStats` and `clearCache` run on a
  background isolate, so they are safe to call from the UI isolate.
- Stickers are decoded at a size close to the widget's size in physical
  pixels (a ladder of 64 to 512 px on the longer side), never at the source
  size. Larger widgets scale the texture up instead of decoding bigger
  frames.
- When more stickers want to play than the tier allows, the ones with the
  most area on screen win.
- Use atlas mode for lists and grids, and keep stickers that are off screen
  out of the tree or under a `TickerMode(enabled: false)`.
- Frames are premultiplied, so stickers composite correctly on any
  background.

## License

This package is BSD-3-Clause licensed. The native libraries it bundles
(libvpx, libwebm, libwebp, giflib, libyuv, lz4) carry their own BSD and MIT
licenses: their full texts follow the package license in [LICENSE](LICENSE),
and [NOTICE](NOTICE) lists each library's copyright line and the patent
grants. Regenerate both with `tool/aggregate_licenses.sh` after changing
`third_party/versions.lock`.
