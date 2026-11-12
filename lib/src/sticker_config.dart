import 'package:flutter/foundation.dart';

import 'sticker_tier.dart';

/// Settings for [WebmStickers.configure]: the frame cache and the device
/// tier.
@immutable
class StickerConfig {
  /// Creates a configuration; a null field keeps its default.
  const StickerConfig({
    this.cacheDirectory,
    this.diskBudgetBytes = defaultDiskBudgetBytes,
    this.tier = StickerTier.auto,
    this.maxFps,
    this.maxConcurrent,
  }) : assert(diskBudgetBytes >= 0, 'diskBudgetBytes must not be negative'),
       assert(
         maxFps == null || (maxFps >= 1 && maxFps <= 240),
         'maxFps must be between 1 and 240',
       ),
       assert(
         maxConcurrent == null || maxConcurrent >= 0,
         'maxConcurrent must not be negative',
       );

  /// The default for [diskBudgetBytes]: 200 MiB.
  static const int defaultDiskBudgetBytes = 200 << 20;

  /// An absolute path for the frame cache, or null for `webm_texture` under
  /// the app's cache directory.
  ///
  /// The plugin creates it, and only ever reads, writes and deletes the
  /// `*.wsc` files it made there.
  final String? cacheDirectory;

  /// The most disk the frame cache may use, in bytes. The least recently used
  /// files are deleted to stay within it: at startup and after each write.
  /// One sticker's file may take at most a quarter of it, and never more than
  /// 64 MiB; a sticker whose frames need more plays without a cache file.
  /// Zero keeps nothing on disk.
  final int diskBudgetBytes;

  /// How much work stickers may do. With [StickerTier.auto], the default, the
  /// plugin detects it from the device's memory, cores and performance class,
  /// and lowers it while Low Power Mode (or battery saver) is on or the
  /// device runs hot. Any other value is used as given, and the device's
  /// state no longer changes it.
  final StickerTier tier;

  /// Overrides the tier's frame rate cap (1..240 frames a second), whatever
  /// the tier. Null keeps the tier's own.
  final int? maxFps;

  /// Overrides the tier's limit on stickers that play at once, whatever the
  /// tier; 0 means no limit. Null keeps the tier's own.
  final int? maxConcurrent;
}
