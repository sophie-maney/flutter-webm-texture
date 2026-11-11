import 'package:flutter/rendering.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

/// Reports how much of [child] shows on screen.
///
/// The check runs after every frame's paint, through [RenderStickerVisibility].
/// [onChanged] receives the visible fraction of the child's area, rounded up
/// to a quarter (0.25, 0.5, 0.75 or 1), or 0 when none of it is on screen.
/// It is called from that post-frame callback, never during build, and only
/// when the value changes, so a scroll reports each sticker at most four
/// times on its way in. Until the first check the child counts as not
/// visible.
class StickerVisibility extends SingleChildRenderObjectWidget {
  /// Creates a detector around [child].
  const StickerVisibility({super.key, required this.onChanged, super.child});

  /// Called with the new visible fraction.
  final ValueChanged<double> onChanged;

  @override
  RenderStickerVisibility createRenderObject(BuildContext context) =>
      RenderStickerVisibility(onChanged: onChanged);

  @override
  void updateRenderObject(
    BuildContext context,
    RenderStickerVisibility renderObject,
  ) {
    renderObject.onChanged = onChanged;
  }
}

/// The render object behind [StickerVisibility].
///
/// It intersects its paint bounds with every clip on the way to the root:
/// each ancestor's [RenderObject.describeApproximatePaintClip] (viewports,
/// `ClipRect`, ...), then the root view. An ancestor that doesn't paint it
/// ([RenderObject.paintsChild] is false: `Offstage`, a sliver scrolled out of
/// its viewport, a kept-alive list item) hides it.
///
/// Scrolling usually repaints none of a list item's own render objects, since
/// each item sits behind a repaint boundary that is only moved, so `paint`
/// can't drive this. Instead every attached instance is checked once after
/// each frame, which is the only time positions change. Nothing is scheduled
/// when no frames are drawn.
class RenderStickerVisibility extends RenderProxyBox {
  /// Creates a detector that reports to [onChanged].
  RenderStickerVisibility({required this.onChanged, RenderBox? child})
    : super(child);

  /// Called with the new visible fraction (see [StickerVisibility]).
  ValueChanged<double> onChanged;

  double _fraction = 0;

  /// Whether any of the child was on screen at the last check.
  bool get visible => _fraction > 0;

  /// The visible fraction as of the last check, in quarters.
  double get visibleFraction => _fraction;

  @override
  void attach(PipelineOwner owner) {
    super.attach(owner);
    _VisibilityTracker.instance.add(this);
  }

  @override
  void detach() {
    _VisibilityTracker.instance.remove(this);
    super.detach();
    _set(0);
  }

  void _set(double fraction) {
    if (fraction == _fraction) return;
    _fraction = fraction;
    onChanged(fraction);
  }

  void _check() {
    final rect = attached ? visibleRect(this) : null;
    _set(
      rect == null
          ? 0
          : quarters(rect.width * rect.height, size.width * size.height),
    );
  }

  /// [visible] / [total] rounded up to a quarter, at least 0.25 when any of
  /// it is visible.
  @visibleForTesting
  static double quarters(double visible, double total) {
    if (!(total > 0) || !(visible > 0)) return 0;
    return ((visible / total) * 4).ceil().clamp(1, 4) / 4;
  }

  /// The part of [box] that is on screen, in [box]'s coordinates, or null
  /// when none of it is. Needs a laid-out tree: call it after layout (in
  /// paint or after the frame).
  static Rect? visibleRect(RenderBox box) {
    if (!box.attached || !box.hasSize || box.size.isEmpty) return null;
    // Box to current ancestor, and the visible part in that ancestor's
    // coordinates.
    var toAncestor = Matrix4.identity();
    var clip = Offset.zero & box.size;
    RenderObject child = box;
    var parent = child.parent;
    while (parent != null) {
      if (!parent.paintsChild(child)) return null;
      final parentClip = parent.describeApproximatePaintClip(child);
      final toParent = Matrix4.identity();
      parent.applyPaintTransform(child, toParent);
      toAncestor = toParent.clone()..multiply(toAncestor);
      // [clip] is in child coordinates; move it into the parent's.
      clip = MatrixUtils.transformRect(toParent, clip);
      if (parentClip != null) clip = clip.intersect(parentClip);
      if (_isEmpty(clip)) return null;
      child = parent;
      parent = child.parent;
    }
    // [child] is the root: a RenderView, whose semanticBounds are the view in
    // physical pixels, like [clip] after its transform.
    clip = clip.intersect(child.semanticBounds);
    if (_isEmpty(clip)) return null;
    final toBox = Matrix4.tryInvert(toAncestor);
    if (toBox == null) return null;
    final local = MatrixUtils.transformRect(
      toBox,
      clip,
    ).intersect(Offset.zero & box.size);
    return _isEmpty(local) ? null : local;
  }

  static bool _isEmpty(Rect r) => !r.isFinite || r.width <= 0 || r.height <= 0;
}

/// Checks every attached [RenderStickerVisibility] after each frame.
class _VisibilityTracker {
  _VisibilityTracker._();

  static final _VisibilityTracker instance = _VisibilityTracker._();

  final Set<RenderStickerVisibility> _boxes = {};
  bool _scheduled = false;

  void add(RenderStickerVisibility box) {
    _boxes.add(box);
    _schedule();
  }

  void remove(RenderStickerVisibility box) => _boxes.remove(box);

  void _schedule() {
    if (_scheduled) return;
    _scheduled = true;
    final scheduler = SchedulerBinding.instance;
    scheduler.addPostFrameCallback(_onFrame, debugLabel: 'StickerVisibility');
    // Attached outside a frame: make sure one comes so the first check runs.
    if (scheduler.schedulerPhase == SchedulerPhase.idle) {
      scheduler.ensureVisualUpdate();
    }
  }

  void _onFrame(Duration _) {
    _scheduled = false;
    if (_boxes.isEmpty) return;
    // A callback may detach boxes (a listener rebuilding the list).
    for (final box in _boxes.toList()) {
      if (_boxes.contains(box)) box._check();
    }
    if (_boxes.isNotEmpty) {
      _scheduled = true;
      SchedulerBinding.instance.addPostFrameCallback(
        _onFrame,
        debugLabel: 'StickerVisibility',
      );
    }
  }
}
