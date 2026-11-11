import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

import 'gen/messages.g.dart';
import 'sticker_bucket.dart';
import 'sticker_registry.dart';
import 'sticker_runtime.dart';
import 'sticker_visibility.dart';

/// Draws the [WebmSticker]s below it through a few shared atlas textures
/// instead of a texture each: the way to show many stickers at once, such as
/// a grid or a picker.
///
/// ```dart
/// StickerAtlasScope(
///   child: GridView.builder(
///     itemBuilder: (context, i) => WebmSticker(source: sources[i]),
///   ),
/// )
/// ```
///
/// The scope owns a native atlas: pages of up to 2048 x [maxPageHeight]
/// physical pixels, each one platform texture that its stickers' frames are
/// drawn into, once per frame tick. A page is only as tall as the rows of
/// stickers it holds, and pages are freed with the scope.
///
/// A sticker takes a slot while some of it is on screen and gives it back
/// when it scrolls away or is removed; its animation is disposed a moment
/// later unless it comes back. Until a slot's first frame reached the page,
/// the sticker shows its `placeholder`. When every page is full ([maxPages]),
/// further stickers play standalone, with a texture each, as they do without
/// a scope.
///
/// Every sticker in the scope plays on its own, as standalone ones do, but
/// stickers showing the same content at the same size in one scope share an
/// animation and a slot.
class StickerAtlasScope extends StatefulWidget {
  /// Creates a scope for the stickers in [child].
  const StickerAtlasScope({
    super.key,
    required this.child,
    this.maxPages = 4,
    this.maxPageHeight = 2048,
  }) : assert(maxPages >= 1 && maxPages <= 16),
       assert(maxPageHeight >= 16 && maxPageHeight <= 2048);

  /// The stickers to draw through the atlas, anywhere below.
  final Widget child;

  /// The most pages (platform textures) the atlas may use, 1 to 16.
  final int maxPages;

  /// The tallest a page may grow, in physical pixels, 16 to 2048. A page's
  /// graphics memory is its width x height x 4 bytes per buffer, and a
  /// platform keeps 2 (iOS) to 7 (Android) buffers per page.
  final int maxPageHeight;

  @override
  State<StickerAtlasScope> createState() => _StickerAtlasScopeState();
}

class _StickerAtlasScopeState extends State<StickerAtlasScope> {
  late final StickerRuntime _runtime = StickerRuntime.instance;
  late final StickerAtlasController _controller = StickerAtlasController(
    host: _runtime.registry.host,
    width: 2048,
    maxHeight: widget.maxPageHeight,
    maxPages: widget.maxPages,
  );

  @override
  void dispose() {
    // Children are gone by now; their animations die with the atlas.
    _runtime.registry.disposeAtlasEntries(_controller);
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => _AtlasScopeMarker(
    controller: _controller,
    child: _PagePump(pages: _controller.sharedPages, child: widget.child),
  );
}

/// Latches each page texture's newest image once per frame, before the
/// slots draw it.
///
/// On Android every un-frozen paint of a texture asks the platform for its
/// latest image (`ImageExternalTexture::Paint`), over JNI: 60 slots of one
/// page would make 60 calls a frame, could each get a different image, and
/// leave images queued, for which the producer schedules more frames. So
/// slots draw page textures frozen, and this paints each page un-frozen
/// first, so every slot shows the image it latched (ADR 0021). iOS behaves
/// the same way with it.
///
/// The engine skips painting under a transparent opacity, so the latch is
/// drawn opaque but invisible: clipped to 1-px boxes that sample only a
/// live slot's gutter, which every present clears. The engine also skips
/// layers outside the screen or an ancestor's clip, so the boxes go at the
/// corners and centre of the part of the scope that is on screen, not of
/// the whole scope: a scope taller than its viewport may show only its
/// middle. After each frame the pump checks that its centre box is still on
/// screen and repaints when it isn't (a scroll moves the pump without
/// repainting it). Until then the corner boxes on the side the content moved
/// to stay on screen, unless the scroll moved it by the whole visible
/// height. The first box painted latches the image; the others find nothing
/// new.
class _PagePump extends SingleChildRenderObjectWidget {
  const _PagePump({required this.pages, super.child});

  /// One drawing per shared page texture: a slot shown on it.
  final ValueListenable<List<StickerAtlasDrawing>> pages;

  @override
  _RenderPagePump createRenderObject(BuildContext context) =>
      _RenderPagePump(pages);

  @override
  void updateRenderObject(BuildContext context, _RenderPagePump renderObject) {
    renderObject.pages = pages;
  }
}

class _RenderPagePump extends RenderProxyBox {
  _RenderPagePump(this._pages);

  ValueListenable<List<StickerAtlasDrawing>> _pages;
  set pages(ValueListenable<List<StickerAtlasDrawing>> value) {
    if (identical(value, _pages)) return;
    if (attached) _pages.removeListener(_changed);
    _pages = value;
    if (attached) _pages.addListener(_changed);
    _changed();
  }

  bool _pumping = false;

  /// The centre latch box of the last paint, in local coordinates; null when
  /// none was painted.
  Rect? _centre;
  bool _checkScheduled = false;

  void _changed() {
    final pumping = _pages.value.isNotEmpty;
    if (pumping != _pumping) {
      _pumping = pumping;
      markNeedsCompositingBitsUpdate();
    }
    markNeedsPaint();
    _scheduleCheck();
  }

  @override
  void attach(PipelineOwner owner) {
    super.attach(owner);
    _pages.addListener(_changed);
    _pumping = _pages.value.isNotEmpty;
    _scheduleCheck();
  }

  @override
  void detach() {
    _pages.removeListener(_changed);
    super.detach();
  }

  @override
  bool get alwaysNeedsCompositing => _pumping;

  /// Checks after every frame while pumping, without asking for frames.
  void _scheduleCheck() {
    if (_checkScheduled || !attached || !_pumping) return;
    _checkScheduled = true;
    SchedulerBinding.instance.addPostFrameCallback(
      _check,
      debugLabel: 'StickerAtlasScope.latch',
    );
  }

  void _check(Duration _) {
    _checkScheduled = false;
    if (!attached || !_pumping) return;
    final visible = RenderStickerVisibility.visibleRect(this);
    final centre = _centre;
    if (visible != null && (centre == null || !visible.overlaps(centre))) {
      markNeedsPaint();
    }
    _scheduleCheck();
  }

  /// Where the latch boxes go when [rect] of the scope is on screen: its
  /// centre first, then its corners.
  @visibleForTesting
  static List<Rect> boxes(Rect rect) {
    if (!(rect.width > 0) || !(rect.height > 0)) return const [];
    Rect at(double x, double y) => Rect.fromLTWH(x, y, 1, 1);
    final centre = Rect.fromCenter(center: rect.center, width: 1, height: 1);
    if (rect.width < 1 || rect.height < 1) return [centre];
    return [
      centre,
      at(rect.left, rect.top),
      at(rect.right - 1, rect.top),
      at(rect.left, rect.bottom - 1),
      at(rect.right - 1, rect.bottom - 1),
    ];
  }

  @override
  void paint(PaintingContext context, Offset offset) {
    final pages = _pages.value;
    _centre = null;
    if (pages.isNotEmpty) {
      // Layout is done, so this is this frame's on-screen part. Off screen
      // (or hidden), boxes anywhere are skipped; the whole scope's serve.
      final local =
          RenderStickerVisibility.visibleRect(this) ?? Offset.zero & size;
      final boxes = _RenderPagePump.boxes(local);
      if (boxes.isNotEmpty) _centre = boxes.first;
      for (final box in boxes) {
        final at = box.shift(offset);
        context.pushClipRect(needsCompositing, Offset.zero, at, (context, _) {
          for (final d in pages) {
            context.addLayer(
              TextureLayer(
                rect: latchRect(d, at),
                textureId: d.textureId,
                filterQuality: FilterQuality.low,
              ),
            );
          }
        });
      }
    }
    super.paint(context, offset);
  }
}

/// Where to draw [d]'s page so that [box] shows only page pixels
/// (x - 1.4 .. x - 0.6, y - 1.4 .. y - 0.6): bilinear samples there read
/// columns x - 2 and x - 1 and rows y - 2 and y - 1 only, the slot's
/// cleared gutter (at least 2 px).
@visibleForTesting
Rect latchRect(StickerAtlasDrawing d, Rect box) {
  const from = 1.4;
  const span = 0.8;
  final sx = box.width / span;
  final sy = box.height / span;
  return Rect.fromLTWH(
    box.left - (d.x - from) * sx,
    box.top - (d.y - from) * sy,
    d.pageWidth * sx,
    d.pageHeight * sy,
  );
}

class _AtlasScopeMarker extends InheritedWidget {
  const _AtlasScopeMarker({required this.controller, required super.child});

  final StickerAtlasController controller;

  @override
  bool updateShouldNotify(_AtlasScopeMarker oldWidget) =>
      !identical(controller, oldWidget.controller);
}

/// The atlas of the nearest [StickerAtlasScope] above [context], or null.
/// Registers [context] for a change of scope.
StickerAtlasController? stickerAtlasOf(BuildContext context) =>
    context.dependOnInheritedWidgetOfExactType<_AtlasScopeMarker>()?.controller;

class _Member {
  _Member(this.handle, this.bucket);

  final AnimationHandleMessage handle;
  final StickerBucket bucket;
  final ValueNotifier<StickerAtlasDrawing?> drawing = ValueNotifier(null);

  /// Waiting for the next attach call.
  bool pending = true;

  /// In an attach call.
  bool sending = false;

  /// The atlas had no room (or no atlas exists): it draws its own texture.
  bool standalone = false;

  AtlasSlotMessage? slot;

  /// The page texture the platform last said shows this slot's frame.
  int shownIn = -1;
}

/// The Dart side of one native atlas (ADR 0020, ADR 0021): batches the
/// animations joining it into one attach per frame, follows the page table,
/// waits for each slot's first frame, and releases replaced textures once no
/// frame draws them. Used from the UI isolate only.
class StickerAtlasController implements StickerAtlasHost {
  /// Creates the controller; the native atlas is created with the first
  /// attach.
  StickerAtlasController({
    required this._host,
    required this.width,
    required this.maxHeight,
    required this.maxPages,
    this.pollInterval = const Duration(milliseconds: 16),
  });

  final WebmTextureHostApi _host;

  /// Page width in physical pixels.
  final int width;

  /// The tallest page.
  final int maxHeight;

  /// The most pages.
  final int maxPages;

  /// How often to ask whether waiting slots reached their page; slower
  /// after many tries.
  final Duration pollInterval;

  final Map<int, _Member> _members = {};
  Map<int, AtlasPageMessage> _pages = {};

  /// Textures the platform retired that may still be drawn.
  final Set<int> _retired = {};

  /// One drawing for each shared page texture that members draw now, by
  /// texture id.
  ValueListenable<List<StickerAtlasDrawing>> get sharedPages => _shared;
  final ValueNotifier<List<StickerAtlasDrawing>> _shared = ValueNotifier(
    const [],
  );

  /// Native atlas id, once created; -1 when creating it failed.
  int? _atlasId;
  bool _disposed = false;

  /// A page may shrink: send an empty attach.
  bool _resync = false;
  bool _flushScheduled = false;
  bool _releaseScheduled = false;
  Timer? _pollTimer;
  int _polls = 0;

  /// Platform calls, one at a time, in order.
  Future<void> _chain = Future.value();
  int _queued = 0;

  /// Platform calls made, for tests.
  @visibleForTesting
  int get debugCalls => _calls;
  int _calls = 0;

  /// The page table as of the last attach or detach.
  @visibleForTesting
  Map<int, AtlasPageMessage> get debugPages => Map.unmodifiable(_pages);

  /// Each attach call so far: how many animations it moved and its round
  /// trip in milliseconds, which includes the platform thread waiting for
  /// each player's frame in flight. For perf tests.
  @visibleForTesting
  final List<(int, double)> debugAttaches = [];

  /// The native atlas id, once created (for `atlasStats` in tests).
  @visibleForTesting
  int? get debugAtlasId => _atlasId;

  /// Retired textures not released yet.
  @visibleForTesting
  Set<int> get debugRetired => Set.unmodifiable(_retired);

  @override
  ValueListenable<StickerAtlasDrawing?> join(
    AnimationHandleMessage handle,
    StickerBucket bucket,
  ) {
    final member = _Member(handle, bucket);
    if (_disposed) return member.drawing;
    _members[handle.animId] = member;
    _scheduleFlush();
    return member.drawing;
  }

  @override
  void leave(int animId) {
    final member = _members.remove(animId);
    if (member == null || _disposed) return;
    _updateShared();
    // Its slot is free now; the page resizes at the next attach.
    if (member.slot != null || member.sending) {
      _resync = true;
      _scheduleFlush();
    }
    _scheduleRelease();
  }

  void _scheduleFlush() {
    if (_flushScheduled || _disposed) return;
    _flushScheduled = true;
    // Once per frame: everything that joined during it goes in one call.
    final binding = SchedulerBinding.instance;
    binding.addPostFrameCallback((_) {
      _flushScheduled = false;
      _flush();
    }, debugLabel: 'StickerAtlasScope.flush');
    binding.ensureVisualUpdate();
  }

  /// Runs [op] after every platform call before it.
  void _enqueue(Future<void> Function() op) {
    _queued++;
    _chain = _chain.then((_) async {
      try {
        if (!_disposed || identical(op, _disposeOp)) await op();
      } catch (e, stack) {
        FlutterError.reportError(
          FlutterErrorDetails(
            exception: e,
            stack: stack,
            library: 'webm_texture',
            context: ErrorDescription('in a StickerAtlasScope platform call'),
          ),
        );
      } finally {
        _queued--;
      }
    });
  }

  Future<void> Function()? _disposeOp;

  void _flush() {
    if (_disposed) return;
    final joining = [
      for (final m in _members.values)
        if (m.pending) m,
    ];
    if (joining.isEmpty && !_resync) return;
    _resync = false;
    for (final m in joining) {
      m
        ..pending = false
        ..sending = true;
    }
    _enqueue(() async {
      final ids = [
        for (final m in joining)
          if (identical(_members[m.handle.animId], m)) m.handle.animId,
      ];
      final atlasId = await _ensureAtlas();
      if (atlasId == null) {
        for (final m in joining) {
          m.sending = false;
          m.standalone = true;
        }
        _refresh();
        return;
      }
      AtlasLayoutMessage layout;
      try {
        _calls++;
        final watch = Stopwatch()..start();
        layout = await _host.attachToAtlas(atlasId, ids);
        debugAttaches.add((ids.length, watch.elapsedMicroseconds / 1000));
      } catch (_) {
        // The engine is going away, say: they keep their own textures.
        for (final m in joining) {
          m.sending = false;
          m.standalone = true;
        }
        _refresh();
        rethrow;
      }
      for (final m in joining) {
        m.sending = false;
      }
      _applyLayout(layout);
    });
  }

  Future<int?> _ensureAtlas() async {
    final id = _atlasId;
    if (id != null) return id < 0 ? null : id;
    try {
      _calls++;
      final created = await _host.createAtlas(
        AtlasOptionsMessage(
          width: width,
          maxHeight: maxHeight,
          maxPages: maxPages,
        ),
      );
      _atlasId = created;
      return created;
    } catch (_) {
      _atlasId = -1;
      return null;
    }
  }

  void _applyLayout(AtlasLayoutMessage layout) {
    for (final s in layout.slots) {
      final m = _members[s.animId];
      // Gone meanwhile: its dispose freed the slot (the resync it asked for
      // resizes the page).
      if (m == null) continue;
      if (s.error != null) {
        m.standalone = true;
      } else {
        m.slot = s;
        m.shownIn = -1;
      }
    }
    _pages = {for (final p in layout.pages) p.pageId: p};
    _retired.addAll(layout.retiredTextureIds);
    _refresh();
  }

  /// Recomputes what every member draws, then polls for slots that wait for
  /// their first frame and releases textures nobody draws.
  void _refresh() {
    if (_disposed) return;
    for (final m in _members.values) {
      final next = _drawingFor(m);
      if (next != null || m.standalone) m.drawing.value = next;
      // Otherwise it keeps what it shows: the placeholder, or a texture
      // that isn't released while it is drawn.
    }
    _updateShared();
    _schedulePoll();
    _scheduleRelease();
  }

  StickerAtlasDrawing? _drawingFor(_Member m) {
    if (m.standalone) {
      final own = m.handle.textureId;
      return own < 0
          ? null
          : StickerAtlasDrawing.whole(own, m.bucket.width, m.bucket.height);
    }
    final slot = m.slot;
    final page = slot == null ? null : _pages[slot.pageId];
    if (slot == null || page == null || page.textureId != m.shownIn) {
      return null;
    }
    return StickerAtlasDrawing(
      textureId: page.textureId,
      pageWidth: page.width,
      pageHeight: page.height,
      x: slot.x,
      y: slot.y,
      width: slot.width,
      height: slot.height,
    );
  }

  void _updateShared() {
    final byTexture = <int, StickerAtlasDrawing>{};
    for (final m in _members.values) {
      final d = m.drawing.value;
      if (d != null && d.shared) byTexture.putIfAbsent(d.textureId, () => d);
    }
    final pages = [
      for (final id in byTexture.keys.toList()..sort()) byTexture[id]!,
    ];
    if (!listEquals(pages, _shared.value)) _shared.value = pages;
  }

  bool _waiting(_Member m) {
    final slot = m.slot;
    if (m.standalone || slot == null) return false;
    final page = _pages[slot.pageId];
    return page != null && page.textureId != m.shownIn;
  }

  void _schedulePoll() {
    if (_disposed || _pollTimer != null) return;
    if (!_members.values.any(_waiting)) {
      _polls = 0;
      return;
    }
    // A page presents within a tick or two of an attach; one that can't
    // (its surface is gone) is asked less often.
    final delay = _polls < 60 ? pollInterval : pollInterval * 16;
    _pollTimer = Timer(delay, () {
      _pollTimer = null;
      _polls++;
      _enqueue(_poll);
    });
  }

  Future<void> _poll() async {
    final atlasId = _atlasId;
    final waiting = _members.values.where(_waiting).toList();
    if (atlasId == null || atlasId < 0 || waiting.isEmpty) {
      _schedulePoll();
      return;
    }
    _calls++;
    final shown = await _host.atlasShownTextures(atlasId, [
      for (final m in waiting) m.handle.animId,
    ]);
    for (var i = 0; i < waiting.length && i < shown.length; i++) {
      waiting[i].shownIn = shown[i];
    }
    _refresh();
  }

  /// Releases retired textures that no member draws, once a frame without
  /// them has been drawn: the engine unregisters a texture after the raster
  /// work queued before it, so the frame that stopped drawing one must have
  /// been submitted first.
  void _scheduleRelease() {
    if (_releaseScheduled || _retired.isEmpty) return;
    _releaseScheduled = true;
    final binding = SchedulerBinding.instance;
    // The first frame rebuilds the widgets that switched; the second starts
    // after it was submitted.
    binding.addPostFrameCallback((_) {
      binding.addPostFrameCallback((_) {
        _releaseScheduled = false;
        _releaseUnused();
      }, debugLabel: 'StickerAtlasScope.release');
      binding.scheduleFrame();
    }, debugLabel: 'StickerAtlasScope.release');
    binding.scheduleFrame();
  }

  void _releaseUnused() {
    final drawn = <int>{
      if (!_disposed)
        for (final m in _members.values)
          if (m.drawing.value case final d?) d.textureId,
    };
    final unused = [
      for (final id in _retired)
        if (!drawn.contains(id)) id,
    ];
    if (unused.isEmpty) return;
    _retired.removeAll(unused);
    _calls++;
    unawaited(_host.releaseTextures(unused).catchError((Object _) {}));
    // Textures still drawn stay until a drawing changes, which schedules
    // this again (_refresh, leave).
  }

  /// Frees the native atlas. Animations still in it must be disposed (the
  /// registry's `disposeAtlasEntries`); their slots' textures are released
  /// once a frame without them was drawn.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _pollTimer?.cancel();
    _pollTimer = null;
    // Widgets may still listen to the members' drawings while they unmount,
    // so those aren't disposed.
    _members.clear();
    Future<void> op() async {
      final atlasId = _atlasId;
      if (atlasId == null || atlasId < 0) {
        _releaseAllRetired();
        return;
      }
      _calls++;
      final layout = await _host.disposeAtlas(atlasId);
      _retired.addAll(layout.retiredTextureIds);
      _releaseAllRetired();
    }

    _disposeOp = op;
    _enqueue(op);
  }

  void _releaseAllRetired() {
    if (_retired.isEmpty) return;
    _releaseScheduled = false;
    _scheduleRelease();
  }

  /// Whether platform calls are queued or running, for tests.
  @visibleForTesting
  bool get debugBusy => _queued > 0;
}

/// Draws a [StickerAtlasDrawing]: its page texture, scaled so that the slot
/// fits the box as [fit] says, and clipped to the slot.
class StickerAtlasSlot extends LeafRenderObjectWidget {
  /// Creates the widget.
  const StickerAtlasSlot({
    super.key,
    required this.drawing,
    this.fit = BoxFit.contain,
  });

  final StickerAtlasDrawing drawing;
  final BoxFit fit;

  @override
  RenderStickerAtlasSlot createRenderObject(BuildContext context) =>
      RenderStickerAtlasSlot(drawing: drawing, fit: fit);

  @override
  void updateRenderObject(
    BuildContext context,
    RenderStickerAtlasSlot renderObject,
  ) {
    renderObject
      ..drawing = drawing
      ..fit = fit;
  }
}

/// The render object behind [StickerAtlasSlot]: one clip layer and one
/// texture layer, no transform layers.
class RenderStickerAtlasSlot extends RenderBox {
  /// Creates the render object.
  RenderStickerAtlasSlot({required this._drawing, required this._fit});

  StickerAtlasDrawing get drawing => _drawing;
  StickerAtlasDrawing _drawing;
  set drawing(StickerAtlasDrawing value) {
    if (value == _drawing) return;
    _drawing = value;
    markNeedsPaint();
  }

  BoxFit get fit => _fit;
  BoxFit _fit;
  set fit(BoxFit value) {
    if (value == _fit) return;
    _fit = value;
    markNeedsPaint();
  }

  @override
  bool get sizedByParent => true;

  @override
  bool get alwaysNeedsCompositing => true;

  @override
  bool get isRepaintBoundary => false;

  @override
  Size computeDryLayout(BoxConstraints constraints) => constraints.biggest;

  /// Where the slot lands in a box of [size] at [offset]: the clip, and the
  /// rect the whole page texture is drawn at.
  @visibleForTesting
  static (Rect clip, Rect page) geometry(
    StickerAtlasDrawing d,
    BoxFit fit,
    Offset offset,
    Size size,
  ) {
    final slot = Size(d.width.toDouble(), d.height.toDouble());
    final fitted = applyBoxFit(fit, slot, size);
    final source = Alignment.center.inscribe(fitted.source, Offset.zero & slot);
    final dest = Alignment.center.inscribe(fitted.destination, offset & size);
    final sx = dest.width / source.width;
    final sy = dest.height / source.height;
    final page = Rect.fromLTWH(
      dest.left - (d.x + source.left) * sx,
      dest.top - (d.y + source.top) * sy,
      d.pageWidth * sx,
      d.pageHeight * sy,
    );
    return (dest, page);
  }

  @override
  void paint(PaintingContext context, Offset offset) {
    final d = _drawing;
    if (size.isEmpty || d.width <= 0 || d.height <= 0) return;
    final (clip, page) = geometry(d, _fit, offset, size);
    if (clip.isEmpty) return;
    _clip.layer = context.pushClipRect(
      needsCompositing,
      Offset.zero,
      clip,
      (context, _) => context.addLayer(
        TextureLayer(
          rect: page,
          textureId: d.textureId,
          // A page's image is latched once a frame by StickerAtlasScope.
          freeze: d.shared,
          filterQuality: FilterQuality.low,
        ),
      ),
      oldLayer: _clip.layer,
    );
  }

  final LayerHandle<ClipRectLayer> _clip = LayerHandle<ClipRectLayer>();

  @override
  void dispose() {
    _clip.layer = null;
    super.dispose();
  }
}
