// Probes every file of the test corpus through dart:ffi on a real device or
// emulator (T1.8). Run with:
//
//   cd example && fvm flutter test integration_test/probe_test.dart -d <device>
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus_expectations.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  late Directory dir;
  late List<Map<String, dynamic>> entries;

  setUpAll(() async {
    final manifest = jsonDecode(
      await rootBundle.loadString('test_assets/manifest.json'),
    ) as Map<String, dynamic>;
    entries = (manifest['files'] as List).cast<Map<String, dynamic>>();
    dir = await Directory.systemTemp.createTemp('wt_corpus');
  });

  tearDownAll(() => dir.delete(recursive: true));

  testWidgets('probes every corpus file from a file and from bytes', (
    tester,
  ) async {
    expect(entries, isNotEmpty);
    for (final entry in entries) {
      final name = entry['name'] as String;
      final data = (await rootBundle.load('test_assets/$name')).buffer
          .asUint8List();
      final file = File('${dir.path}/$name')..writeAsBytesSync(data);

      await expectProbeMatchesManifest(
        entry,
        () => WebmStickers.probe(StickerSource.file(file.path)),
      );
      await expectProbeMatchesManifest(
        entry,
        () => WebmStickers.probe(StickerSource.bytes(data)),
      );
    }
  });

  testWidgets('probe does not block the UI isolate', (tester) async {
    final data = (await rootBundle.load('test_assets/vp9_alpha_512.webm'))
        .buffer
        .asUint8List();
    final futures = [
      for (var i = 0; i < 20; i++)
        WebmStickers.probe(StickerSource.bytes(data)),
    ];
    final infos = await Future.wait(futures);
    expect(infos.every((i) => i.frameCount == 90), isTrue);
  });
}
