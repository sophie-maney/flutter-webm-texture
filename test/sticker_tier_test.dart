import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture/src/sticker_config.dart';
import 'package:webm_texture/src/sticker_registry.dart';
import 'package:webm_texture/src/sticker_source.dart';
import 'package:webm_texture/src/sticker_bucket.dart';
import 'package:webm_texture/src/sticker_tier.dart';

import 'fake_host.dart';

DeviceSignalsMessage _signals({
  int ramMb = 8192,
  int cores = 8,
  bool lowRam = false,
  PerfClassMessage perfClass = PerfClassMessage.unknown,
  bool lowPower = false,
  ThermalStateMessage thermal = ThermalStateMessage.nominal,
}) => DeviceSignalsMessage(
  ramMb: ramMb,
  cores: cores,
  lowRam: lowRam,
  perfClass: perfClass,
  lowPower: lowPower,
  thermal: thermal,
);

void main() {
  group('detect', () {
    test('a flagship is high', () {
      expect(StickerTiers.detect(_signals()), StickerTier.high);
      // iPhone: 6 cores, 4 GB, no performance class.
      expect(
        StickerTiers.detect(_signals(ramMb: 3700, cores: 6)),
        StickerTier.high,
      );
    });

    test('memory sets low and mid', () {
      expect(StickerTiers.detect(_signals(ramMb: 1900)), StickerTier.low);
      expect(StickerTiers.detect(_signals(ramMb: 2559)), StickerTier.low);
      expect(StickerTiers.detect(_signals(ramMb: 2560)), StickerTier.mid);
      expect(StickerTiers.detect(_signals(ramMb: 2800)), StickerTier.mid);
      expect(StickerTiers.detect(_signals(ramMb: 3583)), StickerTier.mid);
      expect(StickerTiers.detect(_signals(ramMb: 3584)), StickerTier.high);
    });

    test('cores set low and mid', () {
      expect(StickerTiers.detect(_signals(cores: 2)), StickerTier.low);
      expect(StickerTiers.detect(_signals(cores: 3)), StickerTier.low);
      expect(StickerTiers.detect(_signals(cores: 4)), StickerTier.mid);
      expect(StickerTiers.detect(_signals(cores: 5)), StickerTier.mid);
      expect(StickerTiers.detect(_signals(cores: 6)), StickerTier.high);
    });

    test('isLowRamDevice and a low performance class are low', () {
      expect(StickerTiers.detect(_signals(lowRam: true)), StickerTier.low);
      expect(
        StickerTiers.detect(_signals(perfClass: PerfClassMessage.low)),
        StickerTier.low,
      );
    });

    test('a mid performance class caps at mid, a high one adds nothing', () {
      expect(
        StickerTiers.detect(_signals(perfClass: PerfClassMessage.mid)),
        StickerTier.mid,
      );
      expect(
        StickerTiers.detect(_signals(perfClass: PerfClassMessage.high)),
        StickerTier.high,
      );
      expect(
        StickerTiers.detect(
          _signals(perfClass: PerfClassMessage.high, ramMb: 2000),
        ),
        StickerTier.low,
      );
    });

    test('Low Power Mode and thermal state do not change the hardware', () {
      expect(
        StickerTiers.detect(
          _signals(lowPower: true, thermal: ThermalStateMessage.critical),
        ),
        StickerTier.high,
      );
    });
  });

  group('effective tier matrix', () {
    const auto = StickerConfig();
    final hardware = {
      StickerTier.high: _signals(),
      StickerTier.mid: _signals(ramMb: 3000),
      StickerTier.low: _signals(ramMb: 1900),
    };
    // thermal x lowPower -> tiers taken off.
    final pressure = <(ThermalStateMessage, bool), int>{
      (ThermalStateMessage.unknown, false): 0,
      (ThermalStateMessage.nominal, false): 0,
      (ThermalStateMessage.fair, false): 0,
      (ThermalStateMessage.serious, false): 1,
      (ThermalStateMessage.critical, false): 2,
      (ThermalStateMessage.unknown, true): 1,
      (ThermalStateMessage.nominal, true): 1,
      (ThermalStateMessage.fair, true): 1,
      (ThermalStateMessage.serious, true): 2,
      (ThermalStateMessage.critical, true): 3,
    };
    const order = [StickerTier.high, StickerTier.mid, StickerTier.low];

    for (final base in order) {
      for (final MapEntry(key: (thermal, lowPower), value: taken)
          in pressure.entries) {
        final expected = order[(order.indexOf(base) + taken).clamp(0, 2)];
        test('$base device, $thermal, lowPower=$lowPower -> $expected', () {
          final s = hardware[base]!
            ..thermal = thermal
            ..lowPower = lowPower;
          expect(StickerTiers.pressure(s), taken);
          expect(StickerTiers.effective(s, auto), expected);
        });
      }
    }

    test('an explicit tier ignores the device', () {
      final s = _signals(ramMb: 1900, lowPower: true);
      expect(
        StickerTiers.effective(s, const StickerConfig(tier: StickerTier.high)),
        StickerTier.high,
      );
      final hot = _signals(thermal: ThermalStateMessage.critical);
      expect(
        StickerTiers.effective(hot, const StickerConfig(tier: StickerTier.mid)),
        StickerTier.mid,
      );
    });
  });

  group('limits', () {
    const auto = StickerConfig();

    test('the tiers of PLAN.md', () {
      expect(
        StickerTiers.limits(StickerTier.high, 8, auto),
        const TierLimits(workerCount: 4, maxFps: 60, maxConcurrent: 0),
      );
      expect(
        StickerTiers.limits(StickerTier.mid, 8, auto),
        const TierLimits(workerCount: 2, maxFps: 30, maxConcurrent: 30),
      );
      expect(
        StickerTiers.limits(StickerTier.low, 8, auto),
        const TierLimits(workerCount: 1, maxFps: 15, maxConcurrent: 8),
      );
    });

    test('workers follow the cores, like the scheduler default', () {
      expect(StickerTiers.limits(StickerTier.high, 2, auto).workerCount, 1);
      expect(StickerTiers.limits(StickerTier.high, 6, auto).workerCount, 3);
      expect(StickerTiers.limits(StickerTier.high, 16, auto).workerCount, 4);
      expect(StickerTiers.limits(StickerTier.mid, 2, auto).workerCount, 1);
    });

    test('maxFps and maxConcurrent override any tier', () {
      const config = StickerConfig(maxFps: 24, maxConcurrent: 0);
      expect(
        StickerTiers.limits(StickerTier.low, 8, config),
        const TierLimits(workerCount: 1, maxFps: 24, maxConcurrent: 0),
      );
      const only = StickerConfig(maxConcurrent: 5);
      expect(
        StickerTiers.limits(StickerTier.mid, 8, only),
        const TierLimits(workerCount: 2, maxFps: 30, maxConcurrent: 5),
      );
    });

    test('a config rejects values the scheduler would', () {
      expect(() => StickerConfig(maxFps: 0), throwsA(isA<AssertionError>()));
      expect(() => StickerConfig(maxFps: 241), throwsA(isA<AssertionError>()));
      expect(
        () => StickerConfig(maxConcurrent: -1),
        throwsA(isA<AssertionError>()),
      );
    });
  });

  group('controller', () {
    late FakeHost host;
    late StreamController<DeviceSignalsMessage> changes;
    late StickerTierController controller;

    setUp(() {
      host = FakeHost();
      changes = StreamController.broadcast();
      controller = StickerTierController(
        host: host,
        signalChanges: () => changes.stream,
      );
    });

    tearDown(() async {
      await controller.dispose();
      await changes.close();
    });

    test('applies the detected tier on start, once', () async {
      host.signals = _signals(ramMb: 3000);
      await controller.start();
      await controller.start();
      expect(controller.tier.value, StickerTier.mid);
      expect(host.schedulerCalls, [(2, 30, 30)]);
    });

    test('a high device sends the scheduler defaults', () async {
      await controller.start();
      expect(controller.tier.value, StickerTier.high);
      expect(host.schedulerCalls, [(4, 60, 0)]);
    });

    test(
      'Low Power Mode downgrades the tier and switching it off restores it',
      () async {
        await controller.start();
        final seen = <StickerTier>[];
        controller.tier.addListener(() => seen.add(controller.tier.value));

        changes.add(_signals(lowPower: true));
        await pumpEventQueue();
        expect(controller.tier.value, StickerTier.mid);
        expect(host.schedulerCalls.last, (2, 30, 30));

        changes.add(
          _signals(lowPower: true, thermal: ThermalStateMessage.serious),
        );
        await pumpEventQueue();
        expect(controller.tier.value, StickerTier.low);
        expect(host.schedulerCalls.last, (1, 15, 8));

        changes.add(_signals());
        await pumpEventQueue();
        expect(controller.tier.value, StickerTier.high);
        expect(host.schedulerCalls.last, (4, 60, 0));
        expect(seen, [StickerTier.mid, StickerTier.low, StickerTier.high]);
      },
    );

    test('an unchanged result is not sent again', () async {
      await controller.start();
      changes
        ..add(_signals(thermal: ThermalStateMessage.fair))
        ..add(_signals());
      await pumpEventQueue();
      expect(host.schedulerCalls, hasLength(1));
    });

    test('configure overrides the tier and ignores later pressure', () async {
      await controller.start();
      await controller.configure(const StickerConfig(tier: StickerTier.low));
      expect(controller.tier.value, StickerTier.low);
      expect(host.schedulerCalls.last, (1, 15, 8));

      changes.add(_signals());
      await pumpEventQueue();
      expect(controller.tier.value, StickerTier.low);
      expect(host.schedulerCalls, hasLength(2));
    });

    test('configure before start is applied by the first read', () async {
      await controller.configure(
        const StickerConfig(maxFps: 24, maxConcurrent: 12),
      );
      expect(host.schedulerCalls, [(4, 24, 12)]);
      expect(
        controller.limits.value,
        const TierLimits(workerCount: 4, maxFps: 24, maxConcurrent: 12),
      );
    });

    test('refresh picks up a change the event channel missed', () async {
      await controller.start();
      host.signals = _signals(lowPower: true);
      await controller.refresh();
      expect(controller.tier.value, StickerTier.mid);
    });

    test('the app resuming refreshes the signals', () async {
      final running = ValueNotifier(true);
      addTearDown(running.dispose);
      controller.attachLifecycle(running);
      await controller.start();
      host.signals = _signals(lowPower: true);
      running.value = false;
      await pumpEventQueue();
      expect(controller.tier.value, StickerTier.high);
      running.value = true;
      await pumpEventQueue();
      expect(controller.tier.value, StickerTier.mid);
    });

    test('a platform without signals leaves the scheduler alone', () async {
      host.signals = null;
      await controller.start();
      expect(host.schedulerCalls, isEmpty);
      expect(controller.tier.value, StickerTier.high);
      expect(controller.limits.value, isNull);
    });

    test('a failing event channel is ignored', () async {
      final failing = StickerTierController(
        host: host,
        signalChanges: () => Stream.error(StateError('no channel')),
      );
      addTearDown(failing.dispose);
      await failing.start();
      await pumpEventQueue();
      expect(failing.tier.value, StickerTier.high);
    });
  });

  test(
    'the registry waits for beforeCreate before the first animation',
    () async {
      final host = FakeHost();
      final gate = Completer<void>();
      final registry = StickerRegistry(
        host: host,
        beforeCreate: () => gate.future,
      );
      addTearDown(registry.dispose);
      final lease = registry.acquire(
        key: 'k',
        source: const FileStickerSource('/a.webm'),
        bucket: const StickerBucket(64, 64),
      );
      await pumpEventQueue();
      expect(host.created, isEmpty);
      gate.complete();
      await lease;
      expect(host.created, hasLength(1));
    },
  );
}
