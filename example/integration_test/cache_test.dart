// The frame cache through the platform glue (T3.2): a sticker that plays
// through writes `<hash>_<w>x<h>_v1.wsc` into the configured directory, a
// second animation of it reuses that file, the disk budget evicts the least
// recently used file, and clearCache() empties the directory. Run with:
//
//   cd example && fvm flutter test integration_test/cache_test.dart -d <device>
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture/webm_texture.dart';

/// 60 frames over 2 s, so one pass takes about two seconds of wall time.
const _clip = 'vp9_opaque_256.webm';
const _bucket = 128;

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final api = WebmTextureHostApi();
  late Directory dir;
  late String originalDirectory;
  late Uint8List data;

  setUpAll(() async {
    data = (await rootBundle.load('test_assets/$_clip')).buffer.asUint8List();
    originalDirectory = await api.configureCache(null);
    dir = await Directory.systemTemp.createTemp('wt_cache_it');
  });

  tearDownAll(() async {
    // Back to where the other tests expect it.
    await WebmStickers.configure(
      StickerConfig(cacheDirectory: originalDirectory),
    );
    await dir.delete(recursive: true);
  });

  setUp(() async {
    await WebmStickers.configure(StickerConfig(cacheDirectory: dir.path));
    await WebmStickers.clearCache();
  });

  List<String> files() =>
      dir
          .listSync()
          .map((e) => e.uri.pathSegments.last)
          .where((n) => n.endsWith('.wsc'))
          .toList()
        ..sort();

  /// Plays [key] for [seconds] (a pass takes two), then disposes it.
  Future<void> play(WidgetTester tester, String key, {int seconds = 4}) async {
    final h = await api.createAnimation(
      key,
      AnimationSourceMessage(bytes: data),
      _bucket,
      _bucket,
      true,
    );
    await api.setActive(h.animId, true);
    for (var i = 0; i < seconds * 4; i++) {
      await tester.pump(const Duration(milliseconds: 250));
    }
    await api.setActive(h.animId, false);
    await api.dispose(h.animId);
  }

  testWidgets('a played-through sticker is cached and reused', (tester) async {
    expect(
      await WebmStickers.cacheStats(),
      const CacheStats(fileCount: 0, totalBytes: 0),
    );
    await play(tester, 'key:cache-test');
    final written = files();
    expect(written, hasLength(1));
    expect(written.single, matches(RegExp(r'^[0-9a-f]{32}_128x128_v1\.wsc$')));
    final stats = await WebmStickers.cacheStats();
    expect(stats.fileCount, 1);
    expect(stats.totalBytes, greaterThan(0));
    expect(
      stats.totalBytes,
      File('${dir.path}/${written.single}').lengthSync(),
    );

    // The same key and size again reads the file: nothing new appears, and
    // the file isn't rewritten with different content.
    await play(tester, 'key:cache-test', seconds: 1);
    expect(files(), written);
    expect(await WebmStickers.cacheStats(), stats);
  });

  testWidgets('another size of the same sticker is another file', (
    tester,
  ) async {
    await play(tester, 'key:size');
    final h = await api.createAnimation(
      'key:size',
      AnimationSourceMessage(bytes: data),
      64,
      64,
      true,
    );
    await api.setActive(h.animId, true);
    for (var i = 0; i < 16; i++) {
      await tester.pump(const Duration(milliseconds: 250));
    }
    await api.dispose(h.animId);
    expect(files().map((n) => n.split('_')[1]).toList()..sort(), [
      '128x128',
      '64x64',
    ]);
  });

  testWidgets('the budget evicts the least recently used file', (tester) async {
    await play(tester, 'key:one');
    final one = (await WebmStickers.cacheStats()).totalBytes;
    expect(one, greaterThan(0));
    await WebmStickers.clearCache();

    // Room for one and a half files: writing the second evicts the first.
    await WebmStickers.configure(
      StickerConfig(cacheDirectory: dir.path, diskBudgetBytes: one * 3 ~/ 2),
    );
    await play(tester, 'key:first');
    final first = files();
    expect(first, hasLength(1));
    await play(tester, 'key:second');
    final second = files();
    expect(second, hasLength(1));
    expect(second, isNot(first));
    expect(
      (await WebmStickers.cacheStats()).totalBytes,
      lessThanOrEqualTo(one * 3 ~/ 2),
    );
  });

  testWidgets('clearCache empties the directory', (tester) async {
    await play(tester, 'key:clear');
    expect(files(), hasLength(1));
    await WebmStickers.clearCache();
    expect(files(), isEmpty);
    expect(
      await WebmStickers.cacheStats(),
      const CacheStats(fileCount: 0, totalBytes: 0),
    );
  });
}
