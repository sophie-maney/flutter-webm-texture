// Drives the platform texture glue (T2.2 on Android, T2.3 on iOS) over the
// Pigeon host API: create from bytes and paths, rejected input, playback
// toggles, create/dispose cycles, and (T2.9) non-looping playback and seeks,
// checked by comparing screenshots with frames decoded over FFI. Run with:
//
//   cd example && fvm flutter test integration_test/texture_test.dart -d <device>
// ignore_for_file: implementation_imports
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:ui' as ui;

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture/src/gen/wt_bindings.g.dart';
import 'package:webm_texture/src/native_library.dart';

import 'screenshots.dart';

/// Opaque, 60 frames over 2 s, 256 x 256, so a screenshot of the texture at
/// 1:1 can be compared with the decoded frame directly.
const _clip = 'vp9_opaque_256.webm';
const _clipFrames = 60;
const _clipSize = 256;

/// Mean absolute RGB difference (0-255) a shown frame may have from its
/// decoded reference. Drawn 1:1, the frame matches exactly on the API 37
/// emulator and the iOS simulator, and to about 2.1 on API 24 (the
/// SurfaceTexture path). Adjacent frames of the clip differ by about 2.5, so
/// [expectFrame] also requires the frame to be the closest candidate.
const _maxFrameError = 3.0;

/// Decodes [frames] of [data] at source size over FFI, as premultiplied
/// RGBA8888, independently of the texture glue.
Future<Map<int, Uint8List>> _decodeFrames(Uint8List data, List<int> frames) =>
    Isolate.run(() {
      final b = loadBindings();
      void check(int status) {
        if (status < 0) throw StateError(statusName(b, status));
      }

      final arena = Arena();
      try {
        final bytes = arena<Uint8>(data.length);
        bytes.asTypedList(data.length).setAll(0, data);
        final source = arena<Pointer<wt_source>>();
        check(b.wt_source_create_memory(bytes, data.length, nullptr, source));
        final anim = arena<Pointer<wt_anim>>();
        final created = b.wt_anim_create(source.value, 0, 0, anim);
        b.wt_source_destroy(source.value);
        check(created);
        try {
          final w = arena<Uint32>();
          final h = arena<Uint32>();
          check(b.wt_anim_get_info(anim.value, nullptr, w, h));
          final size = w.value * h.value * 4;
          final pixels = arena<Uint8>(size);
          final out = <int, Uint8List>{};
          for (final f in frames) {
            check(b.wt_anim_seek(anim.value, f));
            check(
              b.wt_anim_next_frame(
                anim.value,
                pixels,
                size,
                w.value * 4,
                WT_PIXEL_FORMAT_RGBA8888,
                nullptr,
              ),
            );
            out[f] = Uint8List.fromList(pixels.asTypedList(size));
          }
          return out;
        } finally {
          b.wt_anim_destroy(anim.value);
        }
      } finally {
        arena.releaseAll();
      }
    });

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final api = WebmTextureHostApi();
  late Directory dir;
  late List<Map<String, dynamic>> entries;

  setUpAll(() async {
    final manifest = jsonDecode(
      await rootBundle.loadString('test_assets/manifest.json'),
    ) as Map<String, dynamic>;
    entries = (manifest['files'] as List).cast<Map<String, dynamic>>();
    dir = await Directory.systemTemp.createTemp('wt_texture');
    await prepareScreenshots(binding);
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
        final create = api.createAnimation(name, src, 128, 128, true);
        if (expect_ == 'limit_exceeded') {
          await expectLater(create, throwsCode('WT_ERR_LIMIT'), reason: name);
        } else if (expect_ == 'invalid') {
          await expectLater(
            create,
            throwsCode('WT_ERR_INVALID_DATA'),
            reason: name,
          );
        } else if (kind == 'gif') {
          // GIF doesn't decode until T5.2.
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
  });

  testWidgets('rejects malformed requests', (tester) async {
    final data = await load('vp9_alpha_512.webm');
    await expectLater(
      api.createAnimation('none', AnimationSourceMessage(), 128, 128, true),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation(
        'both',
        AnimationSourceMessage(bytes: data, path: '/nonexistent'),
        128,
        128,
        true,
      ),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation(
        'neg',
        AnimationSourceMessage(bytes: data),
        -1,
        1,
        true,
      ),
      throwsCode('WT_ERR_INVALID_ARGUMENT'),
    );
    await expectLater(
      api.createAnimation(
        'missing',
        AnimationSourceMessage(path: '${dir.path}/missing.webm'),
        128,
        128,
        true,
      ),
      throwsCode('WT_ERR_IO'),
    );
  });

  testWidgets('dispose is idempotent and unknown ids are ignored', (
    tester,
  ) async {
    final h = await api.createAnimation(
      'twice',
      AnimationSourceMessage(bytes: await load('vp8_alpha_256.webm')),
      64,
      64,
      true,
    );
    await api.dispose(h.animId);
    await api.dispose(h.animId);
    await api.setActive(h.animId, true);
    await api.seekToFrame(h.animId, 0);
    await api.dispose(-1);
  });

  testWidgets('survives 100 create/play/dispose cycles', (tester) async {
    final src = AnimationSourceMessage(bytes: await load('vp9_alpha_512.webm'));
    for (var i = 0; i < 100; i++) {
      final h = await api.createAnimation('cycle', src, 256, 256, true);
      await api.setActive(h.animId, true);
      await tester.pump(const Duration(milliseconds: 10));
      await api.dispose(h.animId);
    }
  });

  group('loop and seek', () {
    late Uint8List clip;
    late Map<int, Uint8List> refs;
    // The frames the screenshots are told apart from: the ones under test
    // and their neighbours (adjacent frames are the hardest to separate).
    const candidates = [0, 1, 9, 10, 11, 29, 30, 31, 39, 40, 41, 58, 59];

    setUpAll(() async {
      clip = await load(_clip);
      refs = await _decodeFrames(clip, candidates);
    });

    /// Shows each texture at 1:1 physical pixels, side by side.
    Future<void> show(WidgetTester tester, List<int> textureIds) async {
      final dpr = tester.view.devicePixelRatio;
      const top = 300.0; // Physical pixels, below any system bar.
      await tester.pumpWidget(
        Directionality(
          textDirection: TextDirection.ltr,
          child: ColoredBox(
            color: const Color(0xFF000000),
            child: Stack(
              children: [
                for (final (i, id) in textureIds.indexed)
                  Positioned(
                    key: ValueKey(i),
                    left: i * (_clipSize + 32) / dpr,
                    top: top / dpr,
                    width: _clipSize / dpr,
                    height: _clipSize / dpr,
                    child: Texture(textureId: id),
                  ),
              ],
            ),
          ),
        ),
      );
      await tester.pump();
    }

    /// Waits real time while pumping, so the platform keeps compositing.
    Future<void> elapse(WidgetTester tester, Duration d) async {
      final end = DateTime.now().add(d);
      while (DateTime.now().isBefore(end)) {
        await tester.pump(const Duration(milliseconds: 16));
        await Future<void>.delayed(const Duration(milliseconds: 16));
      }
    }

    var shots = 0;

    /// The RGBA pixels of each shown texture, from one screenshot.
    Future<List<Uint8List>> capture(WidgetTester tester, int count) async {
      final png = await binding.takeScreenshot('loop_seek_${shots++}');
      final codec = await ui.instantiateImageCodec(Uint8List.fromList(png));
      final image = (await codec.getNextFrame()).image;
      final data = (await image.toByteData())!.buffer.asUint8List();
      final width = image.width;
      image.dispose();
      codec.dispose();
      final dpr = tester.view.devicePixelRatio;
      return [
        for (var i = 0; i < count; i++)
          () {
            final r = tester.getRect(find.byKey(ValueKey(i)));
            final x = (r.left * dpr).round();
            final y = (r.top * dpr).round();
            final out = Uint8List(_clipSize * _clipSize * 4);
            for (var row = 0; row < _clipSize; row++) {
              final from = ((y + row) * width + x) * 4;
              out.setRange(
                row * _clipSize * 4,
                (row + 1) * _clipSize * 4,
                data,
                from,
              );
            }
            return out;
          }(),
      ];
    }

    /// Mean absolute RGB difference of two RGBA images.
    double diff(Uint8List a, Uint8List b) {
      var sum = 0;
      for (var i = 0; i < a.length; i += 4) {
        sum +=
            (a[i] - b[i]).abs() +
            (a[i + 1] - b[i + 1]).abs() +
            (a[i + 2] - b[i + 2]).abs();
      }
      return sum / (a.length / 4 * 3);
    }

    /// Checks that [shot] shows [frame]: it is within [_maxFrameError] of
    /// that frame's reference and closer to it than to any other candidate.
    void expectFrame(Uint8List shot, int frame, String reason) {
      final errors = {for (final f in candidates) f: diff(shot, refs[f]!)};
      final best = errors.entries.reduce((a, b) => a.value <= b.value ? a : b);
      final detail = errors.entries
          .map((e) => '${e.key}: ${e.value.toStringAsFixed(2)}')
          .join(', ');
      expect(best.key, frame, reason: '$reason; errors {$detail}');
      expect(
        errors[frame],
        lessThanOrEqualTo(_maxFrameError),
        reason: '$reason; errors {$detail}',
      );
    }

    testWidgets('a non-looping animation stops on its last frame', (
      tester,
    ) async {
      final once = await api.createAnimation(
        'once',
        AnimationSourceMessage(bytes: clip),
        0,
        0,
        false,
      );
      final looping = await api.createAnimation(
        'looping',
        AnimationSourceMessage(bytes: clip),
        0,
        0,
        true,
      );
      addTearDown(() async {
        await api.dispose(once.animId);
        await api.dispose(looping.animId);
      });
      await show(tester, [once.textureId, looping.textureId]);
      await elapse(tester, const Duration(milliseconds: 300));
      var shot = await capture(tester, 2);
      expectFrame(shot[0], 0, 'before playing');

      await api.setActive(once.animId, true);
      await api.setActive(looping.animId, true);
      // The clip lasts 2 s.
      await elapse(tester, const Duration(milliseconds: 2800));
      final a = await capture(tester, 2);
      await elapse(tester, const Duration(milliseconds: 500));
      final b = await capture(tester, 2);
      expectFrame(a[0], _clipFrames - 1, 'ended');
      expectFrame(b[0], _clipFrames - 1, 'still ended');
      expect(
        diff(a[1], b[1]),
        greaterThan(1),
        reason: 'the looping copy keeps playing',
      );

      // Pausing and resuming doesn't restart it.
      await api.setActive(once.animId, false);
      await api.setActive(once.animId, true);
      await elapse(tester, const Duration(milliseconds: 400));
      shot = await capture(tester, 2);
      expectFrame(shot[0], _clipFrames - 1, 'resumed after the end');

      // A seek plays it again.
      await api.seekToFrame(once.animId, 0);
      await elapse(tester, const Duration(milliseconds: 500));
      shot = await capture(tester, 2);
      expect(
        diff(shot[0], refs[_clipFrames - 1]!),
        greaterThan(_maxFrameError * 2),
        reason: 'playing again after a seek',
      );
      await elapse(tester, const Duration(milliseconds: 2500));
      shot = await capture(tester, 2);
      expectFrame(shot[0], _clipFrames - 1, 'ended again');
    });

    testWidgets('a seek while paused shows the requested frame', (
      tester,
    ) async {
      final h = await api.createAnimation(
        'seek',
        AnimationSourceMessage(bytes: clip),
        0,
        0,
        true,
      );
      addTearDown(() => api.dispose(h.animId));
      await show(tester, [h.textureId]);
      await elapse(tester, const Duration(milliseconds: 300));
      expectFrame((await capture(tester, 1))[0], 0, 'initial frame');

      // Forward, backward (decoding again from the keyframe), to the last
      // frame, and back to the start.
      for (final frame in [40, 10, 30, 59, 0]) {
        await api.seekToFrame(h.animId, frame);
        await elapse(tester, const Duration(milliseconds: 300));
        expectFrame((await capture(tester, 1))[0], frame, 'seek to $frame');
        await elapse(tester, const Duration(milliseconds: 300));
        expectFrame((await capture(tester, 1))[0], frame, 'held at $frame');
      }

      // While playing, a seek carries on from the requested frame.
      await api.seekToFrame(h.animId, 30);
      await api.setActive(h.animId, true);
      await elapse(tester, const Duration(milliseconds: 300));
      await api.setActive(h.animId, false);
      await elapse(tester, const Duration(milliseconds: 200));
      final shot = (await capture(tester, 1))[0];
      expect(
        diff(shot, refs[30]!),
        greaterThan(_maxFrameError),
        reason: 'played on from frame 30',
      );
      expect(
        diff(shot, refs[30]!),
        lessThan(diff(shot, refs[0]!)),
        reason: 'continued near frame 30, not from the start',
      );

      await expectLater(
        api.seekToFrame(h.animId, _clipFrames),
        throwsCode('WT_ERR_INVALID_ARGUMENT'),
      );
      await expectLater(
        api.seekToFrame(h.animId, -1),
        throwsCode('WT_ERR_INVALID_ARGUMENT'),
      );
    });
  });
}
