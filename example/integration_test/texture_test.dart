// Drives the platform texture glue (T2.3 on iOS; T2.2 enables Android) over
// the Pigeon host API: create from bytes and paths, rejected input, playback
// toggles and create/dispose cycles. Run with:
//
//   cd example && fvm flutter test integration_test/texture_test.dart -d <device>
// ignore_for_file: implementation_imports
import 'dart:convert';
import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';

/// T2.2 removes this once the Android glue lands.
final bool _skip = !Platform.isIOS;

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final api = WebmTextureHostApi();
  late Directory dir;
  late List<Map<String, dynamic>> entries;

  setUpAll(() async {
    final manifest = jsonDecode(
      await rootBundle.loadString('test_assets/manifest.json'),
    ) as Map<String, dynamic>;
    entries = (manifest['files'] as List).cast<Map<String, dynamic>>();
    dir = await Directory.systemTemp.createTemp('wt_texture');
  });

  tearDownAll(() => dir.delete(recursive: true));

  Future<Uint8List> load(String name) async =>
      (await rootBundle.load('test_assets/$name')).buffer.asUint8List();

  Matcher throwsCode(String code) =>
      throwsA(isA<PlatformException>().having((e) => e.code, 'code', code));

  testWidgets('creates a texture from bytes and from a path', (tester) async {
    final ids = <int>{};
    final textures = <int>{};
    for (final entry in entries) {
      final name = entry['name'] as String;
      final kind = entry['kind'] as String;
      final expect_ = entry['expect'] as String;
      final data = await load(name);
      final file = File('${dir.path}/$name')..writeAsBytesSync(data);
      for (final src in [
        AnimationSourceMessage(bytes: data),
        AnimationSourceMessage(path: file.path),
      ]) {
        final create = api.createAnimation(name, src, 128, 128);
        if (expect_ == 'limit_exceeded') {
          await expectLater(create, throwsCode('WT_ERR_LIMIT'), reason: name);
        } else if (expect_ == 'invalid') {
          await expectLater(
            create,
            throwsCode('WT_ERR_INVALID_DATA'),
            reason: name,
          );
        } else if (kind != 'webm') {
          // WebP and GIF don't decode until Phase 5.
          await expectLater(
            create,
            throwsCode('WT_ERR_UNSUPPORTED'),
            reason: name,
          );
        } else {
          final h = await create;
          expect(ids.add(h.animId), isTrue, reason: '$name animId reused');
          expect(textures.add(h.textureId), isTrue, reason: '$name texture');
          await api.setActive(h.animId, true);
          await tester.pump(const Duration(milliseconds: 50));
          await api.setActive(h.animId, false);
          await api.dispose(h.animId);
        }
      }
    }
    expect(ids, isNotEmpty);
  }, skip: _skip);

  testWidgets('rejects malformed requests', (tester) async {
    final data = await load('vp9_alpha_512.webm');
    await expectLater(
      api.createAnimation('none', AnimationSourceMessage(), 128, 128),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation(
        'both',
        AnimationSourceMessage(bytes: data, path: '/nonexistent'),
        128,
        128,
      ),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation('neg', AnimationSourceMessage(bytes: data), -1, 1),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation(
        'missing',
        AnimationSourceMessage(path: '${dir.path}/missing.webm'),
        128,
        128,
      ),
      throwsCode('WT_ERR_IO'),
    );
  }, skip: _skip);

  testWidgets('dispose is idempotent and unknown ids are ignored', (
    tester,
  ) async {
    final h = await api.createAnimation(
      'twice',
      AnimationSourceMessage(bytes: await load('vp8_alpha_256.webm')),
      64,
      64,
    );
    await api.dispose(h.animId);
    await api.dispose(h.animId);
    await api.setActive(h.animId, true);
    await api.dispose(-1);
  }, skip: _skip);

  testWidgets('survives 100 create/play/dispose cycles', (tester) async {
    final src = AnimationSourceMessage(bytes: await load('vp9_alpha_512.webm'));
    for (var i = 0; i < 100; i++) {
      final h = await api.createAnimation('cycle', src, 256, 256);
      await api.setActive(h.animId, true);
      await tester.pump(const Duration(milliseconds: 10));
      await api.dispose(h.animId);
    }
  }, skip: _skip);
}
