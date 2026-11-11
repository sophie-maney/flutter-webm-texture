import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_atlas_scope.dart';
import 'package:webm_texture/src/sticker_registry.dart';
import 'package:webm_texture/src/sticker_runtime.dart';
import 'package:webm_texture/webm_texture.dart';

import 'fake_host.dart';

const _grace = Duration(seconds: 2);
const _placeholder = Key('placeholder');

StickerInfo _info() => const StickerInfo(
  format: StickerFormat.webm,
  codec: StickerCodec.vp9,
  width: 512,
  height: 512,
  frameCount: 30,
  duration: Duration(seconds: 1),
  fps: 30,
  hasAlpha: true,
  hasAudio: false,
  truncated: false,
);

void main() {
  late FakeHost host;
  late StickerRuntime runtime;

  setUp(() {
    host = FakeHost();
    runtime = StickerRuntime(
      registry: StickerRegistry(host: host, gracePeriod: _grace),
      prober: (_) async => _info(),
    );
    StickerRuntime.debugOverride = runtime;
  });

  tearDown(() async {
    StickerRuntime.debugOverride = null;
  });

  Widget app(Widget child) => Directionality(
    textDirection: TextDirection.ltr,
    child: MediaQuery(
      // 64 logical px at 2x: the 128 bucket.
      data: const MediaQueryData(size: Size(800, 600), devicePixelRatio: 2),
      child: child,
    ),
  );

  Widget sticker(String path, {Key? key}) => WebmSticker(
    key: key,
    source: StickerSource.file(path),
    width: 64,
    height: 64,
    placeholder: const SizedBox.expand(key: _placeholder),
  );

  Widget scoped(List<String> paths) => app(
    StickerAtlasScope(
      child: Wrap(
        children: [for (final p in paths) sticker(p, key: ValueKey(p))],
      ),
    ),
  );

  /// Lets probes, platform calls, the per-frame flush, polls and releases run.
  Future<void> settle(WidgetTester tester, [int frames = 12]) async {
    for (var i = 0; i < frames; i++) {
      await tester.pump(const Duration(milliseconds: 20));
    }
  }

  Future<void> finish(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await settle(tester);
    await tester.pump(_grace);
    await settle(tester);
    await runtime.dispose();
  }

  List<RenderStickerAtlasSlot> slots(WidgetTester tester) => tester
      .renderObjectList<RenderStickerAtlasSlot>(find.byType(StickerAtlasSlot))
      .toList();

  testWidgets('batches every sticker that joins in a frame into one attach', (
    tester,
  ) async {
    await tester.pumpWidget(scoped(['/a', '/b', '/c']));
    await settle(tester);
    expect(host.created, hasLength(3));
    expect(host.atlasesCreated, [1]);
    expect(host.attachCalls, [
      [1, 2, 3],
    ]);
    expect(slots(tester), hasLength(3));

    await tester.pumpWidget(scoped(['/a', '/b', '/c', '/d']));
    await settle(tester);
    expect(host.attachCalls, [
      [1, 2, 3],
      [4],
    ]);
    expect(host.atlasesCreated, [1]);
    // Settled: nothing asks for frames (the pages' textures drive them).
    expect(tester.binding.hasScheduledFrame, isFalse);
    await finish(tester);
  });

  testWidgets('latches each page once a frame; slots draw it frozen', (
    tester,
  ) async {
    await tester.pumpWidget(scoped(['/a', '/b', '/c']));
    await settle(tester);
    final page = host.page!.textureId;
    final textures = tester.layers.whereType<TextureLayer>().toList();
    // Un-frozen paints of the page ahead of the slots, clipped to 1-px
    // boxes that show only a slot's gutter.
    final pumps = textures.where((l) => !l.freeze).toList();
    expect(pumps.map((l) => l.textureId), List.filled(5, page));
    expect(textures.take(5), pumps);
    for (final l in pumps) {
      final clip = (l.parent! as ClipRectLayer).clipRect!;
      expect(clip.size, const Size(1, 1));
      // The box maps to page pixels 0.6 .. 1.4 px up and left of the slot.
      final scale = l.rect.width / 2048;
      final px = (clip.left - l.rect.left) / scale;
      final py = (clip.top - l.rect.top) / scale;
      expect(px, closeTo(2 - 1.4, 1e-6));
      expect(py, closeTo(2 - 1.4, 1e-6));
    }
    final slotLayers = textures.where((l) => l.freeze).toList();
    expect(slotLayers.map((l) => l.textureId), [page, page, page]);
    await finish(tester);
  });

  testWidgets('shows the placeholder until the slot reached its page', (
    tester,
  ) async {
    host.shownTextures = {};
    await tester.pumpWidget(scoped(['/a']));
    await settle(tester);
    expect(host.attachCalls, hasLength(1));
    expect(host.shownCalls, isNotEmpty, reason: 'it asks');
    expect(find.byKey(_placeholder), findsOneWidget);
    expect(slots(tester), isEmpty);
    // Its own texture was retired by the attach but is drawn by nobody.
    expect(host.released, [101]);

    final page = host.page!;
    host.shownTextures = {page.textureId};
    await settle(tester);
    expect(find.byKey(_placeholder), findsNothing);
    final d = slots(tester).single.drawing;
    expect(d.textureId, page.textureId);
    expect((d.pageWidth, d.pageHeight), (2048, 144));
    expect((d.x, d.y, d.width, d.height), (2, 2, 128, 128));
    final calls = host.shownCalls.length;
    await settle(tester);
    expect(host.shownCalls.length, calls, reason: 'stops asking');
    await finish(tester);
  });

  testWidgets('lays out from the latest page table, keeping the old texture '
      'until the new one shows the slot', (tester) async {
    await tester.pumpWidget(scoped(['/a']));
    await settle(tester);
    final first = host.page!;
    expect(slots(tester).single.drawing.textureId, first.textureId);

    // The next attach gives the page a new size, and a new texture that
    // hasn't been presented into yet.
    host.resizeNext = true;
    host.shownTextures = {first.textureId};
    await tester.pumpWidget(scoped(['/a', '/b']));
    await settle(tester);
    final second = host.page!;
    expect(second.textureId, isNot(first.textureId));
    // '/a' still draws the old texture, at the old size; '/b' waits.
    expect(slots(tester).map((s) => s.drawing.textureId), [first.textureId]);
    expect(slots(tester).single.drawing.pageHeight, first.height);
    expect(host.released, isNot(contains(first.textureId)));

    host.shownTextures = {second.textureId};
    await settle(tester);
    expect(slots(tester).map((s) => s.drawing.textureId), [
      second.textureId,
      second.textureId,
    ]);
    expect(host.released, contains(first.textureId));
    await finish(tester);
  });

  testWidgets('releases retired textures only after a frame without them', (
    tester,
  ) async {
    host.shownTextures = {};
    await tester.pumpWidget(scoped(['/a']));
    await settle(tester);
    final page = host.page!;

    // A drawn page texture is retired by a resize: not released while it
    // is drawn.
    host.shownTextures = {page.textureId};
    await settle(tester);
    host.resizeNext = true;
    host.shownTextures = {};
    await tester.pumpWidget(scoped(['/a', '/b']));
    await settle(tester);
    expect(host.released, isNot(contains(page.textureId)));

    // The switch happens on a frame; the release waits for the next one.
    host.shownTextures = {host.page!.textureId};
    final releasedBefore = host.released.length;
    for (var i = 0; i < 40; i++) {
      await tester.pump(const Duration(milliseconds: 1));
      if (slots(tester).every((s) => s.drawing.textureId != page.textureId)) {
        break;
      }
    }
    expect(slots(tester).first.drawing.textureId, isNot(page.textureId));
    expect(host.released.length, releasedBefore, reason: 'same frame');
    await tester.pump();
    await tester.pump();
    expect(host.released, contains(page.textureId));
    await finish(tester);
  });

  testWidgets('gives the slot back when the sticker goes away', (tester) async {
    await tester.pumpWidget(scoped(['/a', '/b']));
    await settle(tester);
    expect(host.slots.keys, [1, 2]);

    await tester.pumpWidget(scoped(['/a']));
    await settle(tester);
    expect(host.disposed, isEmpty, reason: 'grace period');
    await tester.pump(_grace);
    await settle(tester);
    expect(host.disposed, [2]);
    expect(host.slots.keys, [1]);
    // The page may shrink now: an empty attach resizes it.
    expect(host.attachCalls.last, isEmpty);
    await finish(tester);
  });

  testWidgets('gives the slot back when the sticker scrolls off screen', (
    tester,
  ) async {
    final controller = ScrollController();
    addTearDown(controller.dispose);
    await tester.pumpWidget(
      app(
        StickerAtlasScope(
          child: ListView(
            controller: controller,
            children: [
              Align(alignment: Alignment.centerLeft, child: sticker('/a')),
              const SizedBox(height: 300),
              Align(alignment: Alignment.centerLeft, child: sticker('/b')),
              const SizedBox(height: 3000),
            ],
          ),
        ),
      ),
    );
    await settle(tester);
    expect(host.slots.keys, [1, 2]);

    // '/b' is still laid out (cache extent) but off screen; '/a' scrolled
    // far away.
    controller.jumpTo(600);
    await settle(tester);
    await tester.pump(_grace);
    await settle(tester);
    expect(host.disposed, containsAll([1, 2]));

    controller.jumpTo(0);
    await settle(tester);
    expect(host.created, hasLength(4), reason: 'back on screen: new ones');
    expect(slots(tester), hasLength(2));
    await finish(tester);
  });

  testWidgets('disposing the scope frees the atlas and its animations', (
    tester,
  ) async {
    await tester.pumpWidget(scoped(['/a', '/b']));
    await settle(tester);
    final page = host.page!;
    await tester.pumpWidget(app(const SizedBox()));
    await settle(tester);
    expect(host.atlasesDisposed, [1]);
    expect(host.disposed, unorderedEquals([1, 2]), reason: 'no grace');
    expect(host.released, contains(page.textureId));
    await finish(tester);
  });

  testWidgets('a full atlas leaves the sticker standalone', (tester) async {
    host.maxSlots = 1;
    await tester.pumpWidget(scoped(['/a', '/b']));
    await settle(tester);
    final drawn = slots(tester).map((s) => s.drawing).toList();
    expect(drawn, hasLength(2));
    // '/b' draws its own texture (102), whole.
    expect(drawn[1], const StickerAtlasDrawing.whole(102, 128, 128));
    expect(host.released, isNot(contains(102)));
    await finish(tester);
  });

  testWidgets('one animation is never standalone and in an atlas at once', (
    tester,
  ) async {
    await tester.pumpWidget(
      app(
        Row(
          children: [
            sticker('/a'),
            StickerAtlasScope(
              child: Row(children: [sticker('/a'), sticker('/a')]),
            ),
          ],
        ),
      ),
    );
    await settle(tester);
    // The standalone one, and one shared by both in the scope.
    expect(host.created, hasLength(2));
    expect(host.attachCalls, [
      [2],
    ]);
    expect(find.byType(Texture), findsOneWidget);
    expect(slots(tester), hasLength(2));
    await finish(tester);
  });

  test('slot geometry: fitted, clipped, page placed around the slot', () {
    const d = StickerAtlasDrawing(
      textureId: 7,
      pageWidth: 2048,
      pageHeight: 288,
      x: 146,
      y: 2,
      width: 128,
      height: 128,
    );
    final (clip, page) = RenderStickerAtlasSlot.geometry(
      d,
      BoxFit.contain,
      const Offset(10, 20),
      const Size(64, 64),
    );
    expect(clip, const Rect.fromLTWH(10, 20, 64, 64));
    expect(page, const Rect.fromLTWH(10 - 73, 20 - 1, 1024, 144));

    // A wide box: contain centres it.
    final (clip2, page2) = RenderStickerAtlasSlot.geometry(
      d,
      BoxFit.contain,
      Offset.zero,
      const Size(128, 64),
    );
    expect(clip2, const Rect.fromLTWH(32, 0, 64, 64));
    expect(page2.left, 32 - 73);
  });
}
