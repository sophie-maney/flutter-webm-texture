// Drives the atlas compositor (T4.2) over the Pigeon host API: 64 animations
// playing through one page texture, the page growing (a new texture) as
// animations join, switching every animation to a texture of its own and
// back, and a slot showing exactly the decoded frame at 1:1. Run with:
//
//   cd example && fvm flutter test integration_test/atlas_test.dart -d <device>
// ignore_for_file: implementation_imports
import 'dart:convert';
import 'dart:ffi';
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
import 'package:webm_texture_example/atlas_demo.dart';

import 'screenshots.dart';

const _count = 64;
const _bucket = 128;

/// 128-px slots have a 144-px pitch: 14 per 2048-px shelf, so 64 of them
/// take 5 shelves.
const _fullHeight = 5 * 144;

/// Opaque, 60 frames over 2 s, 256 x 256: compared 1:1 with a decode.
const _clip = 'vp9_opaque_256.webm';
const _clipSize = 256;
const _maxFrameError = 3.0;

/// Decodes [frames] of [data] at source size over FFI, as RGBA8888.
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
          const size = _clipSize * _clipSize * 4;
          final pixels = arena<Uint8>(size);
          final out = <int, Uint8List>{};
          for (final f in frames) {
            check(b.wt_anim_seek(anim.value, f));
            check(
              b.wt_anim_next_frame(
                anim.value,
                pixels,
                size,
                _clipSize * 4,
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
  final bytes = <String, Uint8List>{};

  setUpAll(() async {
    for (final clip in {...atlasDemoClips, _clip}) {
      bytes[clip] = (await rootBundle.load('test_assets/$clip')).buffer
          .asUint8List();
    }
    await prepareScreenshots(binding);
  });

  /// Waits real time while pumping, so the platform keeps compositing.
  Future<void> elapse(WidgetTester tester, Duration d) async {
    final end = DateTime.now().add(d);
    while (DateTime.now().isBefore(end)) {
      await tester.pump(const Duration(milliseconds: 16));
      await Future<void>.delayed(const Duration(milliseconds: 16));
    }
  }

  Future<List<int>> createAll(int count, int bucket) async => [
    for (var i = 0; i < count; i++)
      (await api.createAnimation(
        'atlas_test_$i',
        AnimationSourceMessage(
          bytes: bytes[atlasDemoClips[i % atlasDemoClips.length]],
        ),
        bucket,
        bucket,
        true,
      )).animId,
  ];

  int presents(AtlasStatsMessage s) =>
      s.pages.fold(0, (sum, p) => sum + p.presents);

  testWidgets('64 animations play through one page texture', (tester) async {
    final ids = await createAll(_count, _bucket);
    final atlasId = await api.createAtlas(
      AtlasOptionsMessage(width: 2048, maxHeight: 2048, maxPages: 4),
    );

    // A first row: one shelf.
    final first = await api.attachToAtlas(atlasId, ids.sublist(0, 8));
    expect(first.slots.map((s) => s.error), everyElement(isNull));
    expect(first.pages, hasLength(1));
    expect(first.pages.single.height, 144);
    expect(first.pages.single.width, 2048);
    // Their own textures, now retired.
    expect(first.retiredTextureIds, hasLength(8));

    // The rest: the page grows, so it gets a new, taller texture.
    final rest = await api.attachToAtlas(atlasId, ids.sublist(8));
    expect(rest.slots.map((s) => s.error), everyElement(isNull));
    expect(rest.pages, hasLength(1));
    final page = rest.pages.single;
    expect(page.height, _fullHeight);
    expect(page.textureId, isNot(first.pages.single.textureId));
    expect(rest.retiredTextureIds, contains(first.pages.single.textureId));
    expect(rest.retiredTextureIds, hasLength(_count - 8 + 1));
    await api.releaseTextures([
      ...first.retiredTextureIds,
      ...rest.retiredTextureIds,
    ]);
    final slots = {
      for (final s in [...first.slots, ...rest.slots]) s.animId: s,
    };
    expect(slots.values.map((s) => s.pageId).toSet(), {0});

    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: GridView.count(
          crossAxisCount: 8,
          children: [
            for (final id in ids) AtlasSlotView(page: page, slot: slots[id]!),
          ],
        ),
      ),
    );
    for (final id in ids) {
      await api.setActive(id, true);
    }
    await elapse(tester, const Duration(milliseconds: 500));
    final before = await api.atlasStats(atlasId);
    await elapse(tester, const Duration(seconds: 2));
    final after = await api.atlasStats(atlasId);

    // One texture for all 64, and new frames keep arriving.
    expect(after.textures, 1);
    expect(after.pages, hasLength(1));
    final p = after.pages.single;
    expect(p.textureId, page.textureId);
    expect(p.liveSlots, _count);
    expect(p.height, _fullHeight);
    expect(p.buffers, greaterThan(0));
    expect(
      p.bufferBytes,
      greaterThanOrEqualTo(p.buffers * 2048 * 4 * p.height),
    );
    expect(p.cpuBytes, greaterThanOrEqualTo(_count * _bucket * _bucket * 4));
    expect(p.presentFailures, 0);
    // At most one present per scheduler tick: the clips' 25-30 fps, or the
    // tier's cap (15 on low-tier emulators once the tier is applied).
    expect(presents(after) - presents(before), greaterThan(10));
    // ignore: avoid_print
    print(
      'ATLAS_TEST ${jsonEncode({'animations': _count, 'pageTextures': after.textures, 'pluginTextures': after.pluginTextures, 'page': '${p.width}x${p.height}', 'buffers': p.buffers, 'bufferBytes': p.bufferBytes, 'cpuBytes': p.cpuBytes, 'presentsPerSecond': (presents(after) - presents(before)) / 2})}',
    );

    // Every animation onto a texture of its own: the page empties and its
    // texture is retired.
    final out = await api.detachFromAtlas(ids);
    expect(out.handles.map((h) => h.textureId), everyElement(isNonNegative));
    expect(out.handles.map((h) => h.textureId).toSet(), hasLength(_count));
    expect(out.pages, isEmpty);
    expect(out.retiredTextureIds, [page.textureId]);
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: GridView.count(
          crossAxisCount: 8,
          children: [
            for (final h in out.handles) Texture(textureId: h.textureId),
          ],
        ),
      ),
    );
    await api.releaseTextures(out.retiredTextureIds);
    await elapse(tester, const Duration(milliseconds: 300));
    expect((await api.atlasStats(atlasId)).textures, 0);

    // And back, in one call: one page again.
    final back = await api.attachToAtlas(atlasId, ids);
    expect(back.slots.map((s) => s.error), everyElement(isNull));
    expect(back.pages, hasLength(1));
    expect(back.pages.single.height, _fullHeight);
    expect(back.retiredTextureIds, hasLength(_count));
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: GridView.count(
          crossAxisCount: 8,
          children: [
            for (final s in back.slots)
              AtlasSlotView(page: back.pages.single, slot: s),
          ],
        ),
      ),
    );
    await api.releaseTextures(back.retiredTextureIds);
    final again = await api.atlasStats(atlasId);
    await elapse(tester, const Duration(seconds: 1));
    expect(
      presents(await api.atlasStats(atlasId)) - presents(again),
      greaterThan(5),
    );

    final gone = await api.disposeAtlas(atlasId);
    expect(gone.retiredTextureIds, [back.pages.single.textureId]);
    await tester.pumpWidget(const SizedBox.shrink());
    await api.releaseTextures(gone.retiredTextureIds);
    for (final id in ids) {
      await api.dispose(id);
    }
  });

  testWidgets('a full atlas reports WT_ERR_LIMIT and keeps the texture', (
    tester,
  ) async {
    // One 256-px page holds two 96-px slots (112-px pitch).
    final ids = await createAll(3, 96);
    final atlasId = await api.createAtlas(
      AtlasOptionsMessage(width: 256, maxHeight: 112, maxPages: 1),
    );
    final layout = await api.attachToAtlas(atlasId, ids);
    expect(layout.slots.map((s) => s.error), [null, null, 'WT_ERR_LIMIT']);
    expect(layout.retiredTextureIds, hasLength(2));
    expect(
      (await api.attachToAtlas(atlasId, [
        ids[0],
        -5,
      ])).slots.map((s) => s.error),
      ['WT_ERR_INVALID_ARGUMENT', 'WT_ERR_INVALID_ARGUMENT'],
    );
    final gone = await api.disposeAtlas(atlasId);
    await api.releaseTextures([
      ...layout.retiredTextureIds,
      ...gone.retiredTextureIds,
    ]);
    // Animations left without a texture get one back.
    final out = await api.detachFromAtlas(ids);
    expect(out.handles.map((h) => h.textureId), everyElement(isNonNegative));
    for (final id in ids) {
      await api.dispose(id);
    }
    await expectLater(
      api.createAtlas(
        AtlasOptionsMessage(width: 8, maxHeight: 64, maxPages: 1),
      ),
      throwsA(isA<PlatformException>()),
    );
  });

  testWidgets('a slot shows the decoded frame at 1:1', (tester) async {
    final refs = await _decodeFrames(bytes[_clip]!, [9, 10, 11]);
    // Fillers take the first shelf, so the clip's slot is off the origin.
    final fillers = await createAll(3, _bucket);
    final clip = (await api.createAnimation(
      _clip,
      AnimationSourceMessage(bytes: bytes[_clip]),
      _clipSize,
      _clipSize,
      true,
    )).animId;
    final atlasId = await api.createAtlas(
      AtlasOptionsMessage(width: 1024, maxHeight: 1024, maxPages: 1),
    );
    final layout = await api.attachToAtlas(atlasId, [...fillers, clip]);
    final slot = layout.slots.last;
    expect(slot.error, isNull);
    expect(slot.y, greaterThan(0));
    await api.seekToFrame(clip, 10);
    await api.releaseTextures(layout.retiredTextureIds);

    final dpr = tester.view.devicePixelRatio;
    const top = 300.0; // Physical pixels, below any system bar.
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: ColoredBox(
          color: const Color(0xFF000000),
          child: Stack(
            children: [
              Positioned(
                key: const ValueKey('slot'),
                left: 0,
                top: top / dpr,
                width: _clipSize / dpr,
                height: _clipSize / dpr,
                child: AtlasSlotView(page: layout.pages.single, slot: slot),
              ),
            ],
          ),
        ),
      ),
    );
    await elapse(tester, const Duration(milliseconds: 500));

    final png = await binding.takeScreenshot('atlas_slot');
    final codec = await ui.instantiateImageCodec(Uint8List.fromList(png));
    final image = (await codec.getNextFrame()).image;
    final data = (await image.toByteData())!.buffer.asUint8List();
    final width = image.width;
    image.dispose();
    codec.dispose();
    final r = tester.getRect(find.byKey(const ValueKey('slot')));
    final x = (r.left * dpr).round();
    final y = (r.top * dpr).round();
    final shot = Uint8List(_clipSize * _clipSize * 4);
    for (var row = 0; row < _clipSize; row++) {
      final from = ((y + row) * width + x) * 4;
      shot.setRange(row * _clipSize * 4, (row + 1) * _clipSize * 4, data, from);
    }
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

    final errors = {for (final f in refs.keys) f: diff(shot, refs[f]!)};
    expect(
      errors[10],
      lessThanOrEqualTo(_maxFrameError),
      reason: 'errors $errors',
    );
    expect(errors[10], lessThan(errors[9]!), reason: 'errors $errors');
    expect(errors[10], lessThan(errors[11]!), reason: 'errors $errors');

    final gone = await api.disposeAtlas(atlasId);
    await tester.pumpWidget(const SizedBox.shrink());
    await api.releaseTextures(gone.retiredTextureIds);
    for (final id in [...fillers, clip]) {
      await api.dispose(id);
    }
  });
}
