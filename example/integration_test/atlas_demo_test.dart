// The example's Atlas (dev) page (T4.2): 64 animations draw through one
// page texture, and every one of them moves to a texture of its own and
// back. Run with:
//
//   cd example && fvm flutter test integration_test/atlas_demo_test.dart -d <device>
//
// With --dart-define=DUMP_SCREENSHOT=true it also prints a screenshot of the
// page, base64 on lines starting with `ATLAS_PNG `, for the PR (decode with
// `grep '^ATLAS_PNG ' log | cut -c11- | tr -d '\n' | base64 -d > shot.png`).
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture_example/atlas_demo.dart';

import 'screenshots.dart';

const _dump = bool.fromEnvironment('DUMP_SCREENSHOT');

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() => prepareScreenshots(binding));

  String info(WidgetTester tester) =>
      tester.widget<Text>(find.byKey(const Key('atlasInfo'))).data ?? '';

  /// Pumps real time until the page's header contains [text].
  Future<void> waitFor(WidgetTester tester, String text) async {
    final end = DateTime.now().add(const Duration(seconds: 20));
    while (!info(tester).contains(text) && DateTime.now().isBefore(end)) {
      await tester.pump(const Duration(milliseconds: 50));
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    expect(info(tester), contains(text));
  }

  Future<void> settle(WidgetTester tester, Duration d) async {
    final end = DateTime.now().add(d);
    while (DateTime.now().isBefore(end)) {
      await tester.pump(const Duration(milliseconds: 16));
      await Future<void>.delayed(const Duration(milliseconds: 16));
    }
  }

  testWidgets('64 animations share one page texture, and switch', (
    tester,
  ) async {
    await tester.pumpWidget(const MaterialApp(home: AtlasDemoPage()));
    await waitFor(tester, '64 animations in atlas mode: 1 platform texture');
    await settle(tester, const Duration(seconds: 2));
    // Refreshed stats: the page keeps presenting.
    expect(info(tester), contains('page 0: 2048x720, 64 slots'));
    // ignore: avoid_print
    print('ATLAS_DEMO ${info(tester).replaceAll('\n', ' | ')}');
    if (_dump) {
      final png = await binding.takeScreenshot('atlas_demo');
      final b64 = base64Encode(png);
      for (var i = 0; i < b64.length; i += 4000) {
        // ignore: avoid_print
        print('ATLAS_PNG ${b64.substring(i, (i + 4000).clamp(0, b64.length))}');
      }
    }

    await tester.tap(find.byKey(const Key('atlasToggle')));
    await waitFor(tester, 'standalone mode: 64 platform textures');
    await settle(tester, const Duration(milliseconds: 500));
    await tester.tap(find.byKey(const Key('atlasToggle')));
    await waitFor(tester, 'atlas mode: 1 platform texture');

    await tester.pumpWidget(const SizedBox.shrink());
    await settle(tester, const Duration(milliseconds: 500));
  });
}
