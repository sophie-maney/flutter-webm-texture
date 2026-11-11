// T3.4: the platform reports its device signals, accepts the scheduler
// options a tier stands for, and the tier follows the config. Low Power Mode
// itself can't be toggled from a test: check it by hand (see
// docs/decisions/0018-device-tiers.md). Run with:
//
//   cd example && fvm flutter test integration_test/tier_test.dart -d <device>
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture/src/sticker_tier.dart';
import 'package:webm_texture/webm_texture.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final host = WebmTextureHostApi();

  testWidgets('deviceSignals reports memory and cores', (tester) async {
    final s = await host.deviceSignals();
    expect(s.ramMb, greaterThan(512));
    expect(s.cores, greaterThanOrEqualTo(1));
    if (Platform.isIOS) {
      expect(s.lowRam, isFalse);
      expect(s.perfClass, PerfClassMessage.unknown);
      expect(s.thermal, isNot(ThermalStateMessage.unknown));
    }
  });

  testWidgets('the scheduler takes tier options and rejects bad ones', (
    tester,
  ) async {
    await host.setSchedulerOptions(1, 15, 8);
    await host.setSchedulerOptions(4, 60, 0);
    for (final bad in [(0, 60, 0), (2, 0, 0), (2, 60, -1), (9, 60, 0)]) {
      await expectLater(
        host.setSchedulerOptions(bad.$1, bad.$2, bad.$3),
        throwsA(
          isA<PlatformException>().having(
            (e) => e.code,
            'code',
            'WT_ERR_INVALID_ARGUMENT',
          ),
        ),
        reason: '$bad',
      );
    }
  });

  testWidgets('configure sets the tier and auto detects it again', (
    tester,
  ) async {
    await WebmStickers.configure(const StickerConfig(tier: StickerTier.low));
    expect(WebmStickers.debugTier.value, StickerTier.low);
    await WebmStickers.configure(const StickerConfig(tier: StickerTier.mid));
    expect(WebmStickers.debugTier.value, StickerTier.mid);

    await WebmStickers.configure(const StickerConfig());
    final s = await host.deviceSignals();
    expect(
      WebmStickers.debugTier.value,
      StickerTiers.effective(s, const StickerConfig()),
    );
    expect(WebmStickers.debugTier.value, isNot(StickerTier.auto));
  });
}
