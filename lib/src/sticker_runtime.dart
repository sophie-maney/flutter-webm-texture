import 'dart:async';
import 'dart:collection';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';

import 'probe.dart';
import 'sticker_cache.dart';
import 'sticker_info.dart';
import 'sticker_registry.dart';
import 'sticker_resolver.dart';
import 'sticker_source.dart';
import 'sticker_tier.dart';

/// Reads a sticker's metadata; [probeSticker] outside tests.
typedef StickerProber = Future<StickerInfo> Function(StickerSource source);

/// Process-wide state behind every `WebmSticker`: the registry, a cache of
/// probe results, and whether the app is in the foreground.
///
/// Used from the UI isolate only.
class StickerRuntime {
  /// Creates a runtime over [registry] and [prober].
  StickerRuntime({
    StickerRegistry? registry,
    StickerProber? prober,
    StickerResolver? resolver,
    StickerCache? cache,
    StickerTierController? tiers,
    this.infoCacheSize = 256,
  }) : tiers = tiers ?? StickerTierController(),
       resolver = resolver ?? StickerResolver(),
       cache = cache ?? StickerCache(),
       _prober = prober ?? probeSticker {
    this.registry = registry ?? StickerRegistry(beforeCreate: _startTiers);
  }

  static StickerRuntime? _default;
  static StickerRuntime? _override;

  /// The runtime widgets use.
  static StickerRuntime get instance =>
      _override ?? (_default ??= StickerRuntime());

  /// Replaces [instance] in tests; null restores the default.
  @visibleForTesting
  static set debugOverride(StickerRuntime? runtime) => _override = runtime;

  /// Shares animations between widgets. Without one given, it waits for the
  /// tier before creating the first animation.
  late final StickerRegistry registry;

  /// Applies the device tier to the native scheduler.
  final StickerTierController tiers;

  /// Loads asset and network sources and names every source's content.
  final StickerResolver resolver;

  /// The frame cache's directory, budget and contents.
  final StickerCache cache;

  final StickerProber _prober;

  /// How many probe results are kept.
  final int infoCacheSize;

  // Insertion order is recency order: a hit is moved to the end.
  final LinkedHashMap<String, Future<StickerInfo>> _infos = LinkedHashMap();

  /// Probes [source] once per [key]. Concurrent calls share one probe.
  /// Failures aren't cached: a file still being written can succeed later.
  Future<StickerInfo> info(String key, StickerSource source) {
    final hit = _infos.remove(key);
    if (hit != null) return _infos[key] = hit;
    final probe = _prober(source);
    _infos[key] = probe;
    if (_infos.length > infoCacheSize) _infos.remove(_infos.keys.first);
    probe.then<void>(
      (_) {},
      onError: (Object _) {
        if (identical(_infos[key], probe)) _infos.remove(key);
      },
    );
    return probe;
  }

  Future<void> _startTiers() {
    // The lifecycle needs a binding, which exists once widgets create
    // animations (but maybe not when configure() runs at startup).
    if (!_tiersAttached) {
      _tiersAttached = true;
      tiers.attachLifecycle(appRunning);
    }
    return tiers.start();
  }

  bool _tiersAttached = false;

  AppLifecycleListener? _lifecycle;
  ValueNotifier<bool>? _appRunning;

  /// False while the app is hidden, paused or detached.
  ValueListenable<bool> get appRunning {
    var running = _appRunning;
    if (running != null) return running;
    running = _appRunning = ValueNotifier(
      _isRunning(WidgetsBinding.instance.lifecycleState),
    );
    _lifecycle = AppLifecycleListener(
      onStateChange: (state) => running!.value = _isRunning(state),
    );
    return running;
  }

  // `inactive` is still on screen (iOS app switcher, a system dialog), and
  // null means no state has been reported yet.
  static bool _isRunning(AppLifecycleState? state) => switch (state) {
    null || AppLifecycleState.resumed || AppLifecycleState.inactive => true,
    AppLifecycleState.hidden ||
    AppLifecycleState.paused ||
    AppLifecycleState.detached => false,
  };

  /// Stops listening to the app lifecycle and disposes [registry].
  Future<void> dispose() {
    _lifecycle?.dispose();
    _lifecycle = null;
    _appRunning?.dispose();
    _appRunning = null;
    _infos.clear();
    unawaited(tiers.dispose());
    return registry.dispose();
  }
}
