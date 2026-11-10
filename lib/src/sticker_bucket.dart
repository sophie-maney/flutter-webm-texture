import 'dart:math' as math;
import 'dart:ui' show Size;

/// A decode size in physical pixels. Animations are decoded and cached at a
/// bucket, never at the source size, so widgets of similar size share one.
class StickerBucket {
  /// Creates a bucket of [width] x [height] pixels.
  const StickerBucket(this.width, this.height);

  /// Width in physical pixels.
  final int width;

  /// Height in physical pixels.
  final int height;

  @override
  bool operator ==(Object other) =>
      other is StickerBucket && other.width == width && other.height == height;

  @override
  int get hashCode => Object.hash(width, height);

  @override
  String toString() => '${width}x$height';
}

/// Maps a widget's size to a [StickerBucket].
abstract final class StickerBuckets {
  /// Allowed lengths of the longer side, in physical pixels.
  static const List<int> ladder = [64, 96, 128, 192, 256, 384, 512];

  /// The smallest ladder step that is at least [pixels]. Anything above the
  /// last step gets the last step: the texture is then scaled up on screen
  /// instead of decoding bigger frames.
  static int roundUp(double pixels) {
    if (!pixels.isFinite || pixels <= 0) {
      throw ArgumentError.value(pixels, 'pixels', 'must be finite and > 0');
    }
    for (final step in ladder) {
      if (pixels <= step) return step;
    }
    return ladder.last;
  }

  /// The bucket for a widget of [logicalSize] at [devicePixelRatio].
  ///
  /// The longer side snaps to [ladder]; the shorter side keeps the aspect
  /// ratio (at least 1 px). Native scales frames to exactly this size, so the
  /// aspect ratio has to be the source's: pass [aspect] (width / height of the
  /// sticker) and the box is first reduced to the largest rectangle of that
  /// aspect that fits inside [logicalSize], like `BoxFit.contain`. Without
  /// [aspect], [logicalSize]'s own ratio is used.
  static StickerBucket forSize(
    Size logicalSize,
    double devicePixelRatio, {
    double? aspect,
  }) {
    if (!devicePixelRatio.isFinite || devicePixelRatio <= 0) {
      throw ArgumentError.value(
        devicePixelRatio,
        'devicePixelRatio',
        'must be finite and > 0',
      );
    }
    var w = logicalSize.width * devicePixelRatio;
    var h = logicalSize.height * devicePixelRatio;
    if (!w.isFinite || !h.isFinite || w <= 0 || h <= 0) {
      throw ArgumentError.value(logicalSize, 'logicalSize', 'must be finite');
    }
    if (aspect != null) {
      if (!aspect.isFinite || aspect <= 0) {
        throw ArgumentError.value(aspect, 'aspect', 'must be finite and > 0');
      }
      if (w / h > aspect) {
        w = h * aspect;
      } else {
        h = w / aspect;
      }
    }
    final longer = roundUp(math.max(w, h));
    final scale = longer / math.max(w, h);
    return w >= h
        ? StickerBucket(longer, math.max(1, (h * scale).round()))
        : StickerBucket(math.max(1, (w * scale).round()), longer);
  }
}
