import 'dart:async';

import 'package:flutter/foundation.dart';

import 'gen/messages.g.dart';
import 'sticker_bucket.dart';
import 'sticker_source.dart';

/// One widget's claim on a shared animation. Give it back with [release].
class StickerLease {
  StickerLease._(this._registry, this._entry);

  final StickerRegistry _registry;
  final _Entry _entry;
  bool _released = false;

  /// Identifies the animation in platform calls.
  int get animId => _entry.handle.animId;

  /// The texture to show. Every lease on the same animation has the same id.
  int get textureId => _entry.handle.textureId;

  /// The decode size of the animation.
  StickerBucket get bucket => _entry.bucket;

  /// Whether [release] was called.
  bool get isReleased => _released;

  /// Gives the claim back. Calling it again does nothing. The animation is
  /// disposed once nobody holds it for [StickerRegistry.gracePeriod].
  void release() {
    if (_released) return;
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
  int refs = 0;
  Timer? graceTimer;
}

@immutable
class _EntryId {
  const _EntryId(this.key, this.bucket);

  final String key;
  final StickerBucket bucket;

  @override
  bool operator ==(Object other) =>
      other is _EntryId && other.key == key && other.bucket == bucket;

  @override
  int get hashCode => Object.hash(key, bucket);
}

/// Shares animations between widgets.
///
/// An animation is identified by (content key, [StickerBucket]). Asking for
/// one that exists, even one waiting out its grace period, returns the same
/// texture; the platform is only called for the first claim and the last
/// release. All methods must be called from the UI isolate.
class StickerRegistry {
  /// Creates a registry over [host] (the real platform API when null).
  ///
  /// An animation nobody holds is kept for [gracePeriod] before the platform
  /// disposes it, so scrolling back doesn't recreate it.
  StickerRegistry({
    WebmTextureHostApi? host,
    this.gracePeriod = const Duration(seconds: 2),
  }) : _host = host ?? WebmTextureHostApi();

  final WebmTextureHostApi _host;

  /// How long an unused animation stays alive.
  final Duration gracePeriod;

  final Map<_EntryId, _Entry> _entries = {};
  bool _disposed = false;

  /// Number of animations the registry holds, including ones in their grace
  /// period.
  @visibleForTesting
  int get entryCount => _entries.length;

  /// Claims the animation for [key] at [bucket], creating it on the platform
  /// if needed. [key] is the content hash or the caller's key; [source] is
  /// only read when the animation has to be created.
  ///
  /// Concurrent calls for the same animation share one platform call. If it
  /// fails, every waiting call gets the error and nothing stays registered.
  Future<StickerLease> acquire({
    required String key,
    required StickerSource source,
    required StickerBucket bucket,
  }) async {
    if (_disposed) throw StateError('StickerRegistry is disposed');
    final id = _EntryId(key, bucket);
    var entry = _entries[id];
    if (entry == null) {
      entry = _Entry(id, bucket);
      _entries[id] = entry;
      final created = entry.created = _host.createAnimation(
        key,
        _toMessage(source),
        bucket.width,
        bucket.height,
      );
      final e = entry;
      // A failed creation must not stay registered, whoever is waiting.
      unawaited(
        created.then(
          (h) => e.handle = h,
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

  void _release(_Entry entry) {
    if (--entry.refs > 0) return;
    if (_disposed) return;
    entry.graceTimer?.cancel();
    entry.graceTimer = Timer(gracePeriod, () {
      entry.graceTimer = null;
      if (entry.refs > 0) return;
      if (identical(_entries[entry.id], entry)) _entries.remove(entry.id);
      unawaited(_disposeOnPlatform(entry));
    });
  }

  Future<void> _disposeOnPlatform(_Entry entry) => entry.created.then<void>(
    (h) => _host.dispose(h.animId).catchError((Object _) {}),
    onError: (Object _) {},
  );

  /// Disposes every animation now, ignoring leases and grace periods. Leases
  /// still held become stale; the registry can't be used afterwards.
  Future<void> dispose() async {
    if (_disposed) return;
    _disposed = true;
    final entries = _entries.values.toList();
    _entries.clear();
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
      };
}
