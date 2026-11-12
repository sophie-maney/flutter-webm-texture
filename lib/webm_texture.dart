import 'package:flutter/foundation.dart';

import 'src/probe.dart';
import 'src/sticker_cache.dart';
import 'src/sticker_config.dart';
import 'src/sticker_tier.dart';
import 'src/sticker_info.dart';
import 'src/sticker_runtime.dart';
import 'src/sticker_source.dart';
import 'webm_texture_platform_interface.dart';

export 'src/sticker_atlas_scope.dart' show StickerAtlasScope;
export 'src/sticker_cache.dart' show CacheStats;
export 'src/sticker_config.dart' show StickerConfig;
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
export 'src/sticker_tier.dart' show StickerTier;
export 'src/webm_sticker.dart'
    show StickerErrorWidgetBuilder, WebmSticker, WebmStickerController;

/// Leftover from the plugin template: reports the platform version.
///
/// Not part of the sticker API; use [WebmSticker] and [WebmStickers].
class WebmTexture {
  /// The platform's version string, such as `Android 14` or `iOS 18.0`.
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

  /// Applies [config]: where the frame cache lives and how big it may grow,
  /// and which tier stickers run at.
  ///
  /// Call it once at startup, before the first sticker is shown; stickers
  /// created earlier keep caching where they began. The cache is evicted down
  /// to [StickerConfig.diskBudgetBytes] right away. Without it the cache
  /// lives in `webm_texture` under the app's cache directory with a 200 MiB
  /// budget, and the tier is detected ([StickerTier.auto]). The tier and its
  /// overrides can be changed later: running stickers follow at once.
  static Future<void> configure(StickerConfig config) async {
    final runtime = StickerRuntime.instance;
    await Future.wait([
      runtime.cache.configure(config),
      runtime.tiers.configure(config),
    ]);
  }

  /// The tier stickers run at now: never [StickerTier.auto]. With
  /// [StickerTier.auto] it follows Low Power Mode and thermal state.
  ///
  /// Meant for debug overlays and tests.
  static ValueListenable<StickerTier> get debugTier =>
      StickerRuntime.instance.tiers.tier;

  /// How many animations the frame cache holds and how much disk they use.
  ///
  /// Runs on a background isolate, so it is safe to call from the UI isolate.
  static Future<CacheStats> cacheStats() =>
      StickerRuntime.instance.cache.stats();

  /// Deletes every cached frame file. Stickers that are playing carry on, and
  /// write their cache again the next time they play through.
  static Future<void> clearCache() => StickerRuntime.instance.cache.clear();

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
