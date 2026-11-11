import 'dart:async';

import 'package:flutter/foundation.dart';

import 'gen/messages.g.dart';
import 'sticker_bucket.dart';
import 'sticker_source.dart';

/// What a sticker in an atlas draws: [textureId] laid out at [pageWidth] x
/// [pageHeight] physical pixels, of which the [width] x [height] rect at
/// ([x], [y]) is the sticker's frame. A texture of the animation's own is a
/// page of its own size with the frame at (0, 0).
@immutable
class StickerAtlasDrawing {
  /// Creates a drawing of a slot of a shared page texture.
  const StickerAtlasDrawing({
    required this.textureId,
    required this.pageWidth,
    required this.pageHeight,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
  }) : shared = true;

  /// A whole texture of [width] x [height], of one animation's own.
  const StickerAtlasDrawing.whole(this.textureId, this.width, this.height)
    : pageWidth = width,
      pageHeight = height,
      x = 0,
      y = 0,
      shared = false;

  /// Whether [textureId] is an atlas page that many slots draw. Those are
  /// drawn frozen, on the image the scope latched for the frame.
  final bool shared;

  final int textureId;
  final int pageWidth;
  final int pageHeight;
  final int x;
  final int y;
  final int width;
  final int height;

  @override
  bool operator ==(Object other) =>
      other is StickerAtlasDrawing &&
      other.textureId == textureId &&
      other.pageWidth == pageWidth &&
      other.pageHeight == pageHeight &&
      other.x == x &&
      other.y == y &&
      other.width == width &&
      other.height == height &&
      other.shared == shared;

  @override
  int get hashCode => Object.hash(
    textureId,
    pageWidth,
    pageHeight,
    x,
    y,
    width,
    height,
    shared,
  );

  @override
  String toString() =>
      'StickerAtlasDrawing(#$textureId ${pageWidth}x$pageHeight, '
      '${width}x$height at $x,$y)';
}

/// An atlas that animations of the registry can be shown through
/// (`StickerAtlasScope`). The registry keys animations by atlas, so one
/// animation is never shown standalone and in an atlas at once: a native
/// player has one target.
abstract class StickerAtlasHost {
  /// The platform created [handle]'s animation for this atlas, with a texture
  /// of its own of [bucket]'s size. Returns what its leases draw: null for
  /// the placeholder.
  ValueListenable<StickerAtlasDrawing?> join(
    AnimationHandleMessage handle,
    StickerBucket bucket,
  );

  /// The platform disposed animation [animId] (which frees its slot).
  void leave(int animId);
}

/// One widget's claim on a shared animation. Give it back with [release].
class StickerLease {
  StickerLease._(this._registry, this._entry);

  final StickerRegistry _registry;
  final _Entry _entry;
  bool _released = false;
  bool _active = false;
  int _visibleArea = 0;

  /// Identifies the animation in platform calls.
  int get animId => _entry.handle.animId;

  /// The texture to show. Every lease on the same animation has the same id.
  /// For an animation in an atlas, its own texture, which the atlas retires:
  /// draw [atlasDrawing] instead.
  int get textureId => _entry.handle.textureId;

  /// What to draw for an animation acquired for an atlas; null for a
  /// standalone one.
  ValueListenable<StickerAtlasDrawing?>? get atlasDrawing => _entry.drawing;

  /// The decode size of the animation.
  StickerBucket get bucket => _entry.bucket;

  /// Whether [release] was called.
  bool get isReleased => _released;

  /// Whether this claim wants the animation to play.
  bool get isActive => _active;

  /// The on-screen area of this claim's widget, in physical pixels.
  int get visibleArea => _visibleArea;

  /// Says whether this claim wants the animation to play. The animation plays
  /// while at least one of its leases is active. Does nothing once released.
  void setActive(bool active) {
    if (_released || active == _active) return;
    _active = active;
    _registry._setLeaseActive(_entry, active, _visibleArea);
  }

  /// Says how much of this claim's widget is on screen, in physical pixels.
  ///
  /// The visible areas of an animation's active leases add up to its
  /// priority: when more animations want to play than the native scheduler's
  /// `maxConcurrent` limit allows, the most visible ones play (ADR 0017).
  /// Does nothing once released.
  void setVisibleArea(int area) {
    if (_released || area == _visibleArea) return;
    final delta = area - _visibleArea;
    _visibleArea = area;
    if (_active) _registry._addPriority(_entry, delta);
  }

  /// Shows [frame] (0-based) on the animation, for every lease on it. A
  /// playing animation carries on from there; a non-looping one that ended
  /// plays again. [frame] must be below the sticker's frame count: the
  /// platform rejects others, and the error is dropped. Does nothing once
  /// released.
  void seekToFrame(int frame) {
    if (_released) return;
    _registry._seek(_entry, frame);
  }

  /// Gives the claim back. Calling it again does nothing. The animation is
  /// disposed once nobody holds it for [StickerRegistry.gracePeriod].
  void release() {
    if (_released) return;
    setActive(false);
    _released = true;
    _registry._release(_entry);
  }
}

class _Entry {
  _Entry(this.id, this.bucket);

  final _EntryId id;
  final StickerBucket bucket;
  late final Future<AnimationHandleMessage> created;
  late AnimationHandleMessage handle;

  /// From [StickerAtlasHost.join], once created for an atlas.
  ValueListenable<StickerAtlasDrawing?>? drawing;

  /// Disposed on the platform (or about to be): never again.
  bool disposed = false;
  int refs = 0;
  int activeLeases = 0;

  /// Sum of the visible areas of the active leases.
  int priority = 0;

  /// The priority the platform last heard.
  int sentPriority = 0;
  Timer? graceTimer;
}

@immutable
class _EntryId {
  const _EntryId(this.key, this.bucket, this.owner, this.loop, this.atlas);

  final String key;
  final StickerBucket bucket;
  final bool loop;

  /// Compared by identity; null for the shared animation.
  final Object? owner;

  /// Compared by identity; null for a standalone animation.
  final StickerAtlasHost? atlas;

  @override
  bool operator ==(Object other) =>
      other is _EntryId &&
      other.key == key &&
      other.bucket == bucket &&
      other.loop == loop &&
      identical(other.owner, owner) &&
      identical(other.atlas, atlas);

  @override
  int get hashCode => Object.hash(
    key,
    bucket,
    loop,
    identityHashCode(owner),
    identityHashCode(atlas),
  );
}

/// Shares animations between widgets.
///
/// An animation is identified by (content key, [StickerBucket], owner, loop,
/// atlas).
/// Asking for one that exists, even one waiting out its grace period, returns
/// the same texture; the platform is only called for the first claim and the
/// last release. It plays while any of its leases is active
/// ([StickerLease.setActive]). All methods must be called from the UI isolate.
class StickerRegistry {
  /// Creates a registry over [host] (the real platform API when null).
  /// [beforeCreate] finishes before any animation is created.
  ///
  /// An animation nobody holds is kept for [gracePeriod] before the platform
  /// disposes it, so scrolling back doesn't recreate it.
  StickerRegistry({
    WebmTextureHostApi? host,
    this.gracePeriod = const Duration(seconds: 2),
    this.beforeCreate,
  }) : _host = host ?? WebmTextureHostApi();

  final WebmTextureHostApi _host;

  /// The platform API animations are created through.
  WebmTextureHostApi get host => _host;

  /// Awaited before each platform creation: the tier's scheduler options must
  /// be in place before the first animation plays.
  final Future<void> Function()? beforeCreate;

  /// How long an unused animation stays alive.
  final Duration gracePeriod;

  final Map<_EntryId, _Entry> _entries = {};
  bool _disposed = false;
  int _activeLeaseCount = 0;
  int _activeAnimationCount = 0;
  bool _publishScheduled = false;
  final ValueNotifier<int> _activeLeases = ValueNotifier(0);
  final ValueNotifier<int> _activeAnimations = ValueNotifier(0);

  /// How many leases are active: widgets that want to play right now.
  ///
  /// Leases change during build and unmount, so the value is published in a
  /// microtask: listeners may call `setState`.
  ValueListenable<int> get activeLeases => _activeLeases;

  /// How many animations are playing on the platform. Published like
  /// [activeLeases].
  ValueListenable<int> get activeAnimations => _activeAnimations;

  void _publishCounts() {
    if (_publishScheduled) return;
    _publishScheduled = true;
    scheduleMicrotask(() {
      _publishScheduled = false;
      _activeLeases.value = _activeLeaseCount;
      _activeAnimations.value = _activeAnimationCount;
    });
  }

  /// Number of animations the registry holds, including ones in their grace
  /// period.
  @visibleForTesting
  int get entryCount => _entries.length;

  /// Claims the animation for [key] at [bucket], creating it on the platform
  /// if needed. [key] names the content (see `ResolvedSticker.key`); [source]
  /// must be resolved and is only read when the animation has to be created.
  ///
  /// Leases with the same non-null [owner] (compared by identity) share an
  /// animation that no other owner sees, so one owner can pause it without
  /// pausing anyone else's. A null [owner] means the shared animation. With
  /// [loop] false the animation stops on its last frame; it is never shared
  /// with a looping one.
  ///
  /// With an [atlas], the animation is shown through it
  /// ([StickerLease.atlasDrawing]) and shared only with leases for the same
  /// atlas.
  ///
  /// Concurrent calls for the same animation share one platform call. If it
  /// fails, every waiting call gets the error and nothing stays registered.
  Future<StickerLease> acquire({
    required String key,
    required StickerSource source,
    required StickerBucket bucket,
    Object? owner,
    bool loop = true,
    StickerAtlasHost? atlas,
  }) async {
    if (_disposed) throw StateError('StickerRegistry is disposed');
    final id = _EntryId(key, bucket, owner, loop, atlas);
    var entry = _entries[id];
    if (entry == null) {
      entry = _Entry(id, bucket);
      _entries[id] = entry;
      final message = _toMessage(source);
      Future<AnimationHandleMessage> create() => _host.createAnimation(
        key,
        message,
        bucket.width,
        bucket.height,
        loop,
      );
      final before = beforeCreate;
      final created = entry.created = before == null
          ? create()
          : before().then((_) => create());
      final e = entry;
      // A failed creation must not stay registered, whoever is waiting.
      unawaited(
        created.then(
          (h) {
            e.handle = h;
            if (atlas != null && identical(_entries[id], e)) {
              e.drawing = atlas.join(h, bucket);
            }
          },
          onError: (Object _) {
            if (identical(_entries[id], e)) _entries.remove(id);
            e.graceTimer?.cancel();
            return null;
          },
        ),
      );
    }
    entry.graceTimer?.cancel();
    entry.graceTimer = null;
    entry.refs++;
    try {
      await entry.created;
    } catch (_) {
      entry.refs--;
      rethrow;
    }
    final lease = StickerLease._(this, entry);
    if (_disposed) {
      // dispose() ran while the platform call was in flight.
      lease._released = true;
      entry.refs--;
      throw StateError('StickerRegistry is disposed');
    }
    return lease;
  }

  void _setLeaseActive(_Entry entry, bool active, int area) {
    _activeLeaseCount += active ? 1 : -1;
    _publishCounts();
    entry.priority += active ? area : -area;
    final before = entry.activeLeases;
    entry.activeLeases += active ? 1 : -1;
    if ((before > 0) == (entry.activeLeases > 0)) {
      _sendPriority(entry);
      return;
    }
    if (_disposed) return;
    _activeAnimationCount += active ? 1 : -1;
    // The rank first, so the scheduler places the animation right away.
    if (active) _sendPriority(entry);
    // Calls on one channel arrive in order, so the last one wins. A failure
    // (the engine is detaching, say) leaves nothing to fix up.
    unawaited(
      _host.setActive(entry.handle.animId, active).catchError((Object _) {}),
    );
  }

  void _addPriority(_Entry entry, int delta) {
    entry.priority += delta;
    _sendPriority(entry);
  }

  /// Tells the platform an active animation's new priority. An inactive one
  /// isn't ranked, so it hears when it becomes active again.
  void _sendPriority(_Entry entry) {
    if (_disposed || entry.activeLeases == 0) return;
    if (entry.priority == entry.sentPriority) return;
    entry.sentPriority = entry.priority;
    unawaited(
      _host
          .setPriority(entry.handle.animId, entry.priority)
          .catchError((Object _) {}),
    );
  }

  void _seek(_Entry entry, int frame) {
    if (_disposed) return;
    unawaited(
      _host.seekToFrame(entry.handle.animId, frame).catchError((Object _) {}),
    );
  }

  void _release(_Entry entry) {
    if (--entry.refs > 0) return;
    if (_disposed || entry.disposed) return;
    entry.graceTimer?.cancel();
    entry.graceTimer = Timer(gracePeriod, () {
      entry.graceTimer = null;
      if (entry.refs > 0) return;
      if (identical(_entries[entry.id], entry)) _entries.remove(entry.id);
      unawaited(_disposeOnPlatform(entry));
    });
  }

  Future<void> _disposeOnPlatform(_Entry entry) {
    if (entry.disposed) return Future.value();
    entry.disposed = true;
    return entry.created.then<void>((h) {
      final done = _host.dispose(h.animId).catchError((Object _) {});
      // Joined atlases hear about it after the call was sent: their next
      // attach or detach resizes the page.
      if (entry.drawing != null) entry.id.atlas?.leave(h.animId);
      return done;
    }, onError: (Object _) {});
  }

  /// Disposes every animation acquired for [atlas] now, ignoring leases and
  /// grace periods (the atlas is going away). Leases still held become
  /// stale.
  void disposeAtlasEntries(StickerAtlasHost atlas) {
    final gone = [
      for (final e in _entries.values)
        if (identical(e.id.atlas, atlas)) e,
    ];
    for (final entry in gone) {
      _entries.remove(entry.id);
      entry.graceTimer?.cancel();
      entry.graceTimer = null;
      unawaited(_disposeOnPlatform(entry));
    }
  }

  /// Disposes every animation now, ignoring leases and grace periods. Leases
  /// still held become stale; the registry can't be used afterwards.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    final entries = _entries.values.toList();
    _entries.clear();
    _activeAnimationCount = 0;
    _publishCounts();
    for (final entry in entries) {
      entry.graceTimer?.cancel();
      entry.graceTimer = null;
    }
    await Future.wait(entries.map(_disposeOnPlatform));
  }

  static AnimationSourceMessage _toMessage(StickerSource source) =>
      switch (source) {
        FileStickerSource(:final path) => AnimationSourceMessage(path: path),
        BytesStickerSource(:final data) => AnimationSourceMessage(bytes: data),
        AssetStickerSource() || NetworkStickerSource() =>
          throw ArgumentError.value(source, 'source', 'must be resolved first'),
      };
}
