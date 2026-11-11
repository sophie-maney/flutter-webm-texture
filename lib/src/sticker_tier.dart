import 'dart:async';

import 'package:flutter/foundation.dart';

import 'gen/messages.g.dart';
import 'sticker_config.dart';

/// How much work the plugin allows itself (PLAN.md, "Tiers").
///
/// With [auto] the plugin picks one from the device and lowers it while the
/// device is under pressure. Any other value is used as given.
enum StickerTier {
  /// Detect the tier from the device and its current state.
  auto,

  /// Source frame rate (at most 60 fps) and no limit on concurrent stickers.
  high,

  /// At most 30 fps and 30 concurrent stickers.
  mid,

  /// At most 15 fps and 8 concurrent stickers; the others show their first
  /// frame.
  low,
}

/// The scheduler tunables a tier stands for (`wt_sched_options`).
@immutable
class TierLimits {
  /// Creates limits.
  const TierLimits({
    required this.workerCount,
    required this.maxFps,
    required this.maxConcurrent,
  });

  /// Decode threads, 1..8.
  final int workerCount;

  /// Most frames a second any sticker shows, 1..240.
  final int maxFps;

  /// Most stickers that play at once; 0 means no limit.
  final int maxConcurrent;

  @override
  bool operator ==(Object other) =>
      other is TierLimits &&
      other.workerCount == workerCount &&
      other.maxFps == maxFps &&
      other.maxConcurrent == maxConcurrent;

  @override
  int get hashCode => Object.hash(workerCount, maxFps, maxConcurrent);

  @override
  String toString() =>
      'TierLimits(workers: $workerCount, maxFps: $maxFps, '
      'maxConcurrent: $maxConcurrent)';
}

/// The tier rules, as pure functions of the device signals (ADR 0018).
abstract final class StickerTiers {
  /// Physical memory below which a device is [StickerTier.low]: devices with
  /// 2 GB report about 1.9 GB.
  static const int lowRamMb = 2560;

  /// Physical memory below which a device is at most [StickerTier.mid]: 3 GB
  /// devices report about 2.8 GB, 4 GB ones about 3.7 GB.
  static const int midRamMb = 3584;

  /// Fewer cores than this make a device [StickerTier.low].
  static const int lowCores = 4;

  /// Fewer cores than this make a device at most [StickerTier.mid].
  static const int midCores = 6;

  /// The tier the hardware allows, whatever the device is doing now. Never
  /// [StickerTier.auto].
  static StickerTier detect(DeviceSignalsMessage s) {
    if (s.lowRam ||
        s.perfClass == PerfClassMessage.low ||
        s.ramMb < lowRamMb ||
        s.cores < lowCores) {
      return StickerTier.low;
    }
    if (s.perfClass == PerfClassMessage.mid ||
        s.ramMb < midRamMb ||
        s.cores < midCores) {
      return StickerTier.mid;
    }
    return StickerTier.high;
  }

  /// How many tiers the device's current state takes off: one for Low Power
  /// Mode (or battery saver), and one for `serious` thermal pressure or two
  /// for `critical`. They add up.
  static int pressure(DeviceSignalsMessage s) {
    final thermal = switch (s.thermal) {
      ThermalStateMessage.serious => 1,
      ThermalStateMessage.critical => 2,
      ThermalStateMessage.unknown ||
      ThermalStateMessage.nominal ||
      ThermalStateMessage.fair => 0,
    };
    return thermal + (s.lowPower ? 1 : 0);
  }

  /// The tier to run at: [detect] lowered by [pressure], but not below
  /// [StickerTier.low]. An explicit [StickerConfig.tier] is used as given.
  static StickerTier effective(DeviceSignalsMessage s, StickerConfig config) {
    if (config.tier != StickerTier.auto) return config.tier;
    // Ordered from best to worst, so a larger index is a lower tier.
    const order = [StickerTier.high, StickerTier.mid, StickerTier.low];
    final index = order.indexOf(detect(s)) + pressure(s);
    return order[index.clamp(0, order.length - 1)];
  }

  /// The scheduler options for [tier] on a device with [cores] cores, with
  /// the overrides of [config] applied.
  static TierLimits limits(StickerTier tier, int cores, StickerConfig config) {
    assert(tier != StickerTier.auto, 'resolve auto first');
    // The scheduler's own default: half the cores, 1 to 4.
    final workers = (cores ~/ 2).clamp(1, 4);
    final base = switch (tier) {
      StickerTier.auto || StickerTier.high => TierLimits(
        workerCount: workers,
        maxFps: 60,
        maxConcurrent: 0,
      ),
      StickerTier.mid => TierLimits(
        workerCount: workers < 2 ? workers : 2,
        maxFps: 30,
        maxConcurrent: 30,
      ),
      StickerTier.low => const TierLimits(
        workerCount: 1,
        maxFps: 15,
        maxConcurrent: 8,
      ),
    };
    return TierLimits(
      workerCount: base.workerCount,
      maxFps: config.maxFps ?? base.maxFps,
      maxConcurrent: config.maxConcurrent ?? base.maxConcurrent,
    );
  }
}

/// Reads the device signals.
typedef DeviceSignalsReader = Future<DeviceSignalsMessage> Function();

/// Opens the stream of signal changes.
typedef DeviceSignalsStream = Stream<DeviceSignalsMessage> Function();

/// Applies the tier's limits to the platform's scheduler, now and whenever
/// the device's state changes. Used from the UI isolate.
///
/// Everything here is best effort: a platform that can't report its signals
/// (or an older one) leaves the scheduler on its defaults.
class StickerTierController {
  /// Creates a controller over [host] (the real platform API when null).
  /// [signalChanges] defaults to the platform's event channel.
  StickerTierController({
    WebmTextureHostApi? host,
    DeviceSignalsStream? signalChanges,
  }) : _host = host ?? WebmTextureHostApi(),
       _signalChanges = signalChanges ?? streamDeviceSignals;

  final WebmTextureHostApi _host;
  final DeviceSignalsStream _signalChanges;

  StickerConfig _config = const StickerConfig();
  DeviceSignalsMessage? _signals;
  TierLimits? _applied;
  Future<void>? _started;
  StreamSubscription<DeviceSignalsMessage>? _subscription;
  ValueListenable<bool>? _appRunning;

  final ValueNotifier<StickerTier> _tier = ValueNotifier(StickerTier.high);
  final ValueNotifier<TierLimits?> _limits = ValueNotifier(null);

  /// The tier in force: never [StickerTier.auto]. It is [StickerTier.high]
  /// until the device has been read.
  ValueListenable<StickerTier> get tier => _tier;

  /// The scheduler options last sent to the platform, or null before that.
  ValueListenable<TierLimits?> get limits => _limits;

  /// Reads the device and applies the tier, once. Later calls return the
  /// same future. Never fails.
  Future<void> start() => _started ??= _start();

  Future<void> _start() async {
    _subscription = _signalChanges().listen(
      (signals) => unawaited(_update(signals)),
      // No event channel (tests, an old platform): polling still works.
      onError: (Object _) {},
    );
    await refresh();
  }

  /// Applies [config]'s tier and overrides. Starts the controller if needed.
  Future<void> configure(StickerConfig config) async {
    _config = config;
    if (_started == null) return start();
    await _started;
    final signals = _signals;
    if (signals != null) await _update(signals);
  }

  /// Rereads the device signals, for changes the event channel can't report.
  Future<void> refresh() async {
    try {
      await _update(await _host.deviceSignals());
    } on Object {
      // Not reported on this platform: keep what is in force.
    }
  }

  /// Calls [refresh] whenever [appRunning] turns true, because Low Power Mode
  /// is switched in another app. Replaces an earlier listenable.
  void attachLifecycle(ValueListenable<bool> appRunning) {
    _appRunning?.removeListener(_onLifecycle);
    _appRunning = appRunning..addListener(_onLifecycle);
  }

  void _onLifecycle() {
    if (_appRunning?.value ?? false) unawaited(refresh());
  }

  Future<void> _update(DeviceSignalsMessage signals) async {
    _signals = signals;
    final tier = StickerTiers.effective(signals, _config);
    final limits = StickerTiers.limits(tier, signals.cores, _config);
    _tier.value = tier;
    if (limits == _applied) return;
    try {
      await _host.setSchedulerOptions(
        limits.workerCount,
        limits.maxFps,
        limits.maxConcurrent,
      );
      _applied = limits;
      _limits.value = limits;
    } on Object {
      // The platform rejected the options: keep the scheduler as it is.
    }
  }

  /// Stops listening.
  Future<void> dispose() async {
    _appRunning?.removeListener(_onLifecycle);
    _appRunning = null;
    await _subscription?.cancel();
    _subscription = null;
    _tier.dispose();
    _limits.dispose();
  }
}
