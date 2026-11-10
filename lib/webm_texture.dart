import 'package:flutter/foundation.dart';

import 'src/probe.dart';
import 'src/sticker_info.dart';
import 'src/sticker_runtime.dart';
import 'src/sticker_source.dart';
import 'webm_texture_platform_interface.dart';

export 'src/sticker_info.dart'
    show
        StickerCodec,
        StickerException,
        StickerFormat,
        StickerInfo,
        StickerLimits;
export 'src/sticker_source.dart'
    show
        AssetStickerSource,
        BytesFetchResult,
        BytesStickerSource,
        FileFetchResult,
        FileStickerSource,
        NetworkStickerSource,
        StickerFetchResult,
        StickerFetcher,
        StickerSource;
export 'src/webm_sticker.dart'
    show StickerErrorWidgetBuilder, WebmSticker, WebmStickerController;

class WebmTexture {
  Future<String?> getPlatformVersion() {
    return WebmTexturePlatform.instance.getPlatformVersion();
  }
}

/// Entry points of the plugin that don't need a widget.
abstract final class WebmStickers {
  /// Handles [StickerSource.network]. Set it once at startup; without one,
  /// network stickers fail with a [StateError] that reaches `errorBuilder`.
  static StickerFetcher? get fetcher =>
      StickerRuntime.instance.resolver.fetcher;
  static set fetcher(StickerFetcher? value) {
    final resolver = StickerRuntime.instance.resolver;
    resolver.fetcher = value;
    resolver.clear();
  }

  /// Reads [source]'s metadata (codec, size, frames, duration, alpha, audio)
  /// without decoding pixels. Assets and network sources are loaded first.
  ///
  /// Runs on a background isolate, so it is safe to call from the UI isolate.
  /// Input is checked against [limits] (the native defaults when null);
  /// throws [StickerException] for a sticker the core rejects, such as
  /// `WT_ERR_LIMIT` for one that is too large.
  static Future<StickerInfo> probe(
    StickerSource source, {
    StickerLimits? limits,
  }) async {
    final resolved = await StickerRuntime.instance.resolver.resolve(source);
    return probeSticker(resolved.source, limits: limits);
  }

  /// How many [WebmSticker]s want to animate right now: playing, on screen,
  /// under an enabled `TickerMode`, with the app in the foreground.
  ///
  /// Meant for debug overlays and tests.
  static ValueListenable<int> get debugActiveCount =>
      StickerRuntime.instance.registry.activeLeases;
}
