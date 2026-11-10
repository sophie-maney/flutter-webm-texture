import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'gen/wt_bindings.g.dart' as wt;
import 'sticker_bucket.dart';
import 'sticker_info.dart';
import 'sticker_registry.dart';
import 'sticker_runtime.dart';
import 'sticker_source.dart';
import 'sticker_visibility.dart';

/// Builds what a [WebmSticker] shows when its sticker can't be played.
///
/// [error] is usually a [StickerException]; its `name` says why (such as
/// `WT_ERR_LIMIT` for a sticker over the limits).
typedef StickerErrorWidgetBuilder = Widget Function(
  BuildContext context,
  Object error,
);

/// Plays and pauses the [WebmSticker]s it is given to.
///
/// Stickers with a controller get an animation of their own, shared only with
/// other stickers on the same controller, so pausing one never pauses a copy
/// elsewhere on screen.
class WebmStickerController extends ChangeNotifier {
  /// Creates a controller that starts playing, or paused if [playing] is
  /// false.
  WebmStickerController({this._playing = true});

  bool _playing;

  /// Whether the stickers should play. They still only advance while on
  /// screen, under an enabled `TickerMode`, with the app in the foreground.
  bool get isPlaying => _playing;

  /// Resumes playback from the frame that is showing.
  void play() => _setPlaying(true);

  /// Holds the frame that is showing.
  void pause() => _setPlaying(false);

  void _setPlaying(bool playing) {
    if (playing == _playing) return;
    _playing = playing;
    notifyListeners();
  }
}

/// An animated sticker: VP9/VP8 WebM with alpha, animated WebP or GIF.
///
/// Frames are decoded natively at a size bucket close to the widget's size in
/// physical pixels and drawn through a platform `Texture`. Widgets showing
/// the same content at the same bucket share one animation.
///
/// The sticker advances only while all of these hold: it plays ([autoplay],
/// or [controller]'s `isPlaying`), some of it is on screen, `TickerMode` is
/// enabled, and the app is in the foreground. Otherwise it holds its current
/// frame.
class WebmSticker extends StatefulWidget {
  /// Creates a sticker for [source].
  const WebmSticker({
    super.key,
    required this.source,
    this.width,
    this.height,
    this.fit = BoxFit.contain,
    this.autoplay = true,
    this.controller,
    this.placeholder,
    this.errorBuilder,
  });

  /// Where the encoded sticker comes from.
  final StickerSource source;

  /// The width in logical pixels. When null it follows from [height] and the
  /// sticker's aspect ratio, or from the sticker's size in pixels, within
  /// the incoming constraints.
  final double? width;

  /// The height in logical pixels; see [width].
  final double? height;

  /// How the sticker is inscribed into the box.
  final BoxFit fit;

  /// Whether to play without a [controller]. When false the sticker shows its
  /// first frame. Ignored when [controller] is set.
  final bool autoplay;

  /// Plays and pauses the sticker; see [WebmStickerController].
  final WebmStickerController? controller;

  /// Shown until the first frame is ready. Empty when null.
  final Widget? placeholder;

  /// Shown instead of the sticker when it can't be played. Empty when null.
  final StickerErrorWidgetBuilder? errorBuilder;

  @override
  State<WebmSticker> createState() => _WebmStickerState();
}

class _WebmStickerState extends State<WebmSticker> {
  late final StickerRuntime _runtime = StickerRuntime.instance;
  late final ValueListenable<bool> _appRunning = _runtime.appRunning;

  /// Identifies this widget's private animation when it isn't shared.
  final Object _privateOwner = Object();

  ResolvedSticker? _resolved;
  StickerInfo? _info;
  Object? _error;

  /// Bumped when a new source makes earlier probes stale.
  int _sourceGeneration = 0;

  /// Bumped when a new source or owner makes earlier acquisitions stale.
  int _generation = 0;

  StickerLease? _lease;
  (int, StickerBucket)? _leaseFor;
  (int, StickerBucket)? _pending;

  bool _visible = false;
  bool _tickerEnabled = true;
  double _devicePixelRatio = 1;

  Object? get _owner =>
      widget.controller ?? (widget.autoplay ? null : _privateOwner);

  bool get _playing => widget.controller?.isPlaying ?? widget.autoplay;

  bool get _shouldAnimate =>
      _playing && _visible && _tickerEnabled && _appRunning.value;

  @override
  void initState() {
    super.initState();
    _appRunning.addListener(_updateActive);
    widget.controller?.addListener(_updateActive);
    _load();
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _tickerEnabled = TickerMode.valuesOf(context).enabled;
    _devicePixelRatio =
        MediaQuery.maybeDevicePixelRatioOf(context) ??
        View.of(context).devicePixelRatio;
    _updateActive();
  }

  @override
  void didUpdateWidget(WebmSticker oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.controller != widget.controller) {
      oldWidget.controller?.removeListener(_updateActive);
      widget.controller?.addListener(_updateActive);
    }
    final oldOwner =
        oldWidget.controller ?? (oldWidget.autoplay ? null : _privateOwner);
    if (widget.source != oldWidget.source) {
      _releaseLease();
      _load();
    } else if (!identical(oldOwner, _owner)) {
      // Keep showing the current lease until the new animation is ready.
      _generation++;
      _pending = null;
    }
    _updateActive();
  }

  @override
  void dispose() {
    _sourceGeneration++;
    _generation++;
    _appRunning.removeListener(_updateActive);
    widget.controller?.removeListener(_updateActive);
    _releaseLease();
    super.dispose();
  }

  void _load() {
    final generation = ++_sourceGeneration;
    _generation++;
    _resolved = null;
    _info = null;
    _error = null;
    _pending = null;
    _runtime.resolver
        .resolve(widget.source)
        .then((resolved) {
          if (!mounted || generation != _sourceGeneration) {
            return Future<StickerInfo?>.value();
          }
          _resolved = resolved;
          return _runtime.info(resolved.key, resolved.source);
        })
        .then<void>(
          (info) {
            if (info == null || !mounted || generation != _sourceGeneration) {
              return;
            }
            if (info.width <= 0 || info.height <= 0) {
              setState(
                () => _error = const StickerException(
                  wt.WT_ERR_INVALID_DATA,
                  'WT_ERR_INVALID_DATA',
                  'empty frame size',
                ),
              );
            } else {
              setState(() => _info = info);
            }
          },
          onError: (Object error) {
            if (!mounted || generation != _sourceGeneration) return;
            setState(() => _error = _describe(error));
          },
        );
  }

  void _releaseLease() {
    _lease?.release();
    _lease = null;
    _leaseFor = null;
    _pending = null;
  }

  void _updateActive() => _lease?.setActive(_shouldAnimate);

  void _onVisibilityChanged(bool visible) {
    _visible = visible;
    _updateActive();
  }

  /// Acquires the animation at [bucket] for the current source and owner,
  /// unless it is already held or on its way. Called during layout, so it
  /// must not mark anything dirty synchronously.
  void _ensureBucket(StickerBucket bucket) {
    final resolved = _resolved;
    if (resolved == null) return;
    final want = (_generation, bucket);
    if (_pending == want) return;
    if (_pending == null && _leaseFor == want) return;
    _pending = want;
    final generation = _generation;
    _runtime.registry
        .acquire(
          key: resolved.key,
          source: resolved.source,
          bucket: bucket,
          owner: _owner,
        )
        .then<void>(
          (lease) {
            if (!mounted || generation != _generation || _pending != want) {
              lease.release();
              return;
            }
            final old = _lease;
            _lease = lease;
            _leaseFor = want;
            _pending = null;
            _updateActive();
            old?.release();
            setState(() {});
          },
          onError: (Object error) {
            if (!mounted || generation != _generation || _pending != want) {
              return;
            }
            _pending = null;
            setState(() => _error = _describe(error));
          },
        );
  }

  /// The logical size of the box, before the sticker is fitted into it.
  Size _boxSize(BoxConstraints constraints) {
    final info = _info;
    final w = widget.width;
    final h = widget.height;
    if (info == null) return constraints.constrain(Size(w ?? 0, h ?? 0));
    final aspect = info.width / info.height;
    final size = switch ((w, h)) {
      (final w?, final h?) => Size(w, h),
      (final w?, null) => Size(w, w / aspect),
      (null, final h?) => Size(h * aspect, h),
      (null, null) => Size(info.width.toDouble(), info.height.toDouble()),
    };
    return constraints.constrainSizeAndAttemptToPreserveAspectRatio(size);
  }

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, constraints) {
      final box = _boxSize(constraints);
      return SizedBox.fromSize(
        size: box,
        child: StickerVisibility(
          onChanged: _onVisibilityChanged,
          child: _content(context, box),
        ),
      );
    },
  );

  Widget _content(BuildContext context, Size box) {
    final error = _error;
    if (error != null) {
      return widget.errorBuilder?.call(context, error) ??
          const SizedBox.expand();
    }
    final info = _info;
    if (info != null &&
        box.width.isFinite &&
        box.height.isFinite &&
        box.width > 0 &&
        box.height > 0) {
      _ensureBucket(
        StickerBuckets.forSize(
          box,
          _devicePixelRatio,
          aspect: info.width / info.height,
        ),
      );
    }
    final lease = _lease;
    if (lease == null) return widget.placeholder ?? const SizedBox.expand();
    // The texture holds exactly bucket.width x bucket.height pixels, which
    // has the sticker's aspect ratio.
    return FittedBox(
      fit: widget.fit,
      clipBehavior: Clip.hardEdge,
      child: SizedBox(
        width: lease.bucket.width.toDouble(),
        height: lease.bucket.height.toDouble(),
        child: Texture(textureId: lease.textureId),
      ),
    );
  }

  static Object _describe(Object error) => switch (error) {
    PlatformException(:final code, :final message) => StickerException(
      _statusCodes[code] ?? 0,
      code,
      message,
    ),
    _ => error,
  };

  static const Map<String, int> _statusCodes = {
    'WT_ERR_INVALID_ARGUMENT': wt.WT_ERR_INVALID_ARGUMENT,
    'WT_ERR_UNIMPLEMENTED': wt.WT_ERR_UNIMPLEMENTED,
    'WT_ERR_INVALID_DATA': wt.WT_ERR_INVALID_DATA,
    'WT_ERR_UNSUPPORTED': wt.WT_ERR_UNSUPPORTED,
    'WT_ERR_TRUNCATED': wt.WT_ERR_TRUNCATED,
    'WT_ERR_LIMIT': wt.WT_ERR_LIMIT,
    'WT_ERR_NO_MEMORY': wt.WT_ERR_NO_MEMORY,
    'WT_ERR_IO': wt.WT_ERR_IO,
    'WT_ERR_DECODE': wt.WT_ERR_DECODE,
    'WT_ERR_INTERNAL': wt.WT_ERR_INTERNAL,
  };
}
