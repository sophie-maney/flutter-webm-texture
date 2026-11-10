import 'src/probe.dart';
import 'src/sticker_info.dart';
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
    show BytesStickerSource, FileStickerSource, StickerSource;

class WebmTexture {
  Future<String?> getPlatformVersion() {
    return WebmTexturePlatform.instance.getPlatformVersion();
  }
}

/// Entry points of the plugin that don't need a widget.
abstract final class WebmStickers {
  /// Reads [source]'s metadata (codec, size, frames, duration, alpha, audio)
  /// without decoding pixels.
  ///
  /// Runs on a background isolate, so it is safe to call from the UI isolate.
  /// Input is checked against [limits] (the native defaults when null);
  /// throws [StickerException] for a sticker the core rejects, such as
  /// `WT_ERR_LIMIT` for one that is too large.
  static Future<StickerInfo> probe(
    StickerSource source, {
    StickerLimits? limits,
  }) => probeSticker(source, limits: limits);
}
