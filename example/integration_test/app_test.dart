// ignore_for_file: implementation_imports
// T2.8: end-to-end checks of the example app on a device or simulator.
//
// - A screenshot golden of the checkerboard page, per platform, compared with
//   a tolerance (emulator GPUs and font rasterisation differ slightly).
// - T4.3: the Stress page in atlas mode (StickerAtlasScope) against the same
//   page standalone, and against its own golden: no slot bleeds into its
//   neighbour's cell.
// - The middle of a scope taller than the screen animates (the latch boxes
//   follow the part of the scope that shows).
// - Playback advances on screen, holds when paused, and holds while the
//   sticker is scrolled off screen or the app is backgrounded.
//
//   cd example && fvm flutter test integration_test/app_test.dart -d <device>
//
// Goldens live in `integration_test/goldens/<platform>/` and are bundled as
// assets. To (re)record them, run with `--dart-define=UPDATE_GOLDENS=true`
// and pipe the output through `tool/extract_goldens.dart`:
//
//   fvm flutter test integration_test/app_test.dart -d <device> \
//     --dart-define=UPDATE_GOLDENS=true | fvm dart run ../tool/extract_goldens.dart
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/sticker_atlas_scope.dart';
import 'package:webm_texture/webm_texture.dart';
import 'package:webm_texture_example/checkerboard_page.dart';
import 'package:webm_texture_example/corpus.dart';
import 'package:webm_texture_example/main.dart';
import 'package:webm_texture_example/stress_page.dart';

import 'screenshots.dart';

const _updateGoldens = bool.fromEnvironment('UPDATE_GOLDENS');

/// Fraction of pixels allowed to differ by more than [_channelTolerance].
const _maxDiffFraction = 0.01;

/// Per-channel difference (0-255) that still counts as the same colour.
const _channelTolerance = 24;

final _platform = Platform.isIOS ? 'ios' : 'android';

late IntegrationTestWidgetsFlutterBinding _binding;

/// A decoded screenshot.
class _Shot {
  _Shot(this.width, this.height, this.rgba);

  final int width;
  final int height;
  final Uint8List rgba;
}

Future<_Shot> _decode(List<int> png) async {
  final codec = await ui.instantiateImageCodec(Uint8List.fromList(png));
  final image = (await codec.getNextFrame()).image;
  final data = (await image.toByteData())!;
  final shot = _Shot(
    image.width,
    image.height,
    data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes),
  );
  image.dispose();
  codec.dispose();
  return shot;
}

/// The [w] x [h] region of [s] at ([x], [y]).
_Shot _crop(_Shot s, int x, int y, int w, int h) {
  expect(y + h, lessThanOrEqualTo(s.height), reason: 'screen too short');
  final out = Uint8List(w * h * 4);
  for (var row = 0; row < h; row++) {
    final from = ((y + row) * s.width + x) * 4;
    out.setRange(row * w * 4, (row + 1) * w * 4, s.rgba, from);
  }
  return _Shot(w, h, out);
}

/// Box-filters [s] down by 2 so goldens stay small.
_Shot _half(_Shot s) {
  final w = s.width ~/ 2, h = s.height ~/ 2;
  final out = Uint8List(w * h * 4);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      for (var c = 0; c < 4; c++) {
        var sum = 0;
        for (var dy = 0; dy < 2; dy++) {
          for (var dx = 0; dx < 2; dx++) {
            sum += s.rgba[((2 * y + dy) * s.width + 2 * x + dx) * 4 + c];
          }
        }
        out[(y * w + x) * 4 + c] = sum ~/ 4;
      }
    }
  }
  return _Shot(w, h, out);
}

Future<List<int>> _encodePng(_Shot s) async {
  final c = Completer<ui.Image>();
  ui.decodeImageFromPixels(
    s.rgba,
    s.width,
    s.height,
    ui.PixelFormat.rgba8888,
    c.complete,
  );
  final image = await c.future;
  final data = (await image.toByteData(format: ui.ImageByteFormat.png))!;
  image.dispose();
  return data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes);
}

var _shotCounter = 0;

Future<(_Shot, List<int>)> _screenshot() async {
  final png = await _binding.takeScreenshot('shot_${_shotCounter++}');
  return (await _decode(png), png);
}

/// Fraction of pixels in [a] and [b] (cropped to [rect], in physical pixels,
/// or whole when null) whose channels differ by more than [_channelTolerance].
double _diff(_Shot a, _Shot b, [Rect? rect]) {
  expect((a.width, a.height), (b.width, b.height), reason: 'screenshot size');
  final r =
      rect ?? Rect.fromLTWH(0, 0, a.width.toDouble(), a.height.toDouble());
  final x0 = r.left.floor().clamp(0, a.width);
  final x1 = r.right.ceil().clamp(0, a.width);
  final y0 = r.top.floor().clamp(0, a.height);
  final y1 = r.bottom.ceil().clamp(0, a.height);
  var differing = 0;
  var total = 0;
  for (var y = y0; y < y1; y++) {
    for (var x = x0; x < x1; x++) {
      final i = (y * a.width + x) * 4;
      total++;
      for (var c = 0; c < 4; c++) {
        if ((a.rgba[i + c] - b.rgba[i + c]).abs() > _channelTolerance) {
          differing++;
          break;
        }
      }
    }
  }
  return total == 0 ? 0 : differing / total;
}

/// Prints [png] as `GOLDEN <name> <i>/<n> <base64>` lines for
/// `tool/extract_goldens.dart`.
void _printGolden(String name, List<int> png) {
  final b64 = base64Encode(png);
  const chunk = 600;
  final n = (b64.length / chunk).ceil();
  for (var i = 0; i < n; i++) {
    final end = (i + 1) * chunk > b64.length ? b64.length : (i + 1) * chunk;
    // ignore: avoid_print
    print(
      'GOLDEN $_platform/$name ${i + 1}/$n ${b64.substring(i * chunk, end)}',
    );
  }
}

/// Pumps until [count] textures exist and had time to draw a frame.
Future<void> _waitForTextures(WidgetTester tester, int count) async {
  for (var i = 0; i < 200; i++) {
    await tester.pump(const Duration(milliseconds: 50));
    if (find.byType(Texture).evaluate().length >= count) break;
  }
  expect(find.byType(Texture), findsAtLeastNWidgets(count));
  // Let the first frames reach the screen.
  await tester.pump(const Duration(milliseconds: 300));
}

/// Waits real time (screenshots are taken from the live surface) while
/// pumping frames.
Future<void> _elapse(WidgetTester tester, Duration d) async {
  final end = DateTime.now().add(d);
  while (DateTime.now().isBefore(end)) {
    await tester.pump(const Duration(milliseconds: 16));
    await Future<void>.delayed(const Duration(milliseconds: 16));
  }
}

/// The physical-pixel rect of the first [WebmSticker].
Rect _stickerRect(WidgetTester tester, [int index = 0]) {
  final finder = find.byType(WebmSticker).at(index);
  final dpr = tester.view.devicePixelRatio;
  final r = tester.getRect(finder);
  return Rect.fromLTRB(
    r.left * dpr,
    r.top * dpr,
    r.right * dpr,
    r.bottom * dpr,
  ).deflate(4 * dpr);
}

void main() {
  _binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() => prepareScreenshots(_binding));

  testWidgets('checkerboard page matches its golden', (tester) async {
    // Disabled tickers hold every sticker on its first frame, which makes
    // the screenshot deterministic.
    await tester.pumpWidget(
      const MaterialApp(
        home: TickerMode(enabled: false, child: CheckerboardPage()),
      ),
    );
    await _waitForTextures(tester, checkerboardStickers.length);
    await _elapse(tester, const Duration(milliseconds: 500));

    final (full, _) = await _screenshot();
    // Only the page body, at a fixed size: the system bars and the screen
    // height differ between emulator API levels.
    final dpr = tester.view.devicePixelRatio;
    final body = tester.getRect(find.byKey(const Key('checkerboard')));
    final w = (body.width * dpr).floor() ~/ 2 * 2;
    final shot = _half(
      _crop(
        full,
        (body.left * dpr).round(),
        (body.top * dpr).round(),
        w,
        w * 3 ~/ 2,
      ),
    );
    final png = await _encodePng(shot);
    const name = 'checkerboard.png';
    if (_updateGoldens) {
      _printGolden(name, png);
      return;
    }
    final golden = await _decode(
      (await rootBundle.load('integration_test/goldens/$_platform/$name'))
          .buffer
          .asUint8List(),
    );
    final diff = _diff(shot, golden);
    if (diff > _maxDiffFraction) _printGolden('actual_$name', png);
    expect(
      diff,
      lessThanOrEqualTo(_maxDiffFraction),
      reason: 'fraction of differing pixels vs the $_platform golden',
    );
  });

  testWidgets('stress page in atlas mode matches standalone and its golden', (
    tester,
  ) async {
    // The top 4 x 4 cells of the grid, every sticker on its first frame.
    final ticking = ValueNotifier(false);
    addTearDown(ticking.dispose);
    late Rect crop;
    Future<(_Shot, List<int>)> shoot({required bool atlas}) async {
      await tester.pumpWidget(
        MaterialApp(
          home: ValueListenableBuilder<bool>(
            valueListenable: ticking,
            builder: (_, on, child) => TickerMode(enabled: on, child: child!),
            child: StressPage(
              key: ValueKey(atlas),
              initiallyAtlas: atlas,
              columns: 4,
            ),
          ),
        ),
      );
      final drawn = find.byType(atlas ? StickerAtlasSlot : Texture);
      var last = -1;
      for (var i = 0; i < 300; i++) {
        await tester.pump(const Duration(milliseconds: 50));
        final n = drawn.evaluate().length;
        // Every visible cell draws, and nothing changed for a while.
        if (n >= 16 && n == last && i > 20) break;
        last = n;
      }
      expect(drawn, findsAtLeastNWidgets(16));
      await _elapse(tester, const Duration(milliseconds: 700));
      final (full, _) = await _screenshot();
      final dpr = tester.view.devicePixelRatio;
      final grid = tester.getRect(find.byKey(const Key('stressGrid')));
      final w = (grid.width * dpr).floor() ~/ 2 * 2;
      crop = Rect.fromLTWH(
        (grid.left * dpr).roundToDouble(),
        (grid.top * dpr).roundToDouble(),
        w.toDouble(),
        w.toDouble(),
      );
      final shot = _half(
        _crop(full, crop.left.toInt(), crop.top.toInt(), w, w),
      );
      return (shot, await _encodePng(shot));
    }

    final (standalone, standalonePng) = await shoot(atlas: false);
    final (atlas, atlasPng) = await shoot(atlas: true);

    // Playing, the slots animate.
    ticking.value = true;
    await tester.pump(const Duration(milliseconds: 300));
    final (a, _) = await _screenshot();
    await _elapse(tester, const Duration(milliseconds: 400));
    final (b, _) = await _screenshot();
    expect(
      _diff(a, b, crop),
      greaterThan(0.001),
      reason: 'playing atlas stickers should change',
    );
    await tester.pumpWidget(const SizedBox());

    final vsStandalone = _diff(atlas, standalone);
    if (vsStandalone > _maxDiffFraction) {
      _printGolden('actual_stress_standalone.png', standalonePng);
      _printGolden('actual_stress_atlas.png', atlasPng);
    }
    expect(
      vsStandalone,
      lessThanOrEqualTo(_maxDiffFraction),
      reason: 'fraction of pixels differing between atlas and standalone',
    );

    const name = 'stress_atlas.png';
    if (_updateGoldens) {
      _printGolden(name, atlasPng);
      return;
    }
    final golden = await _decode(
      (await rootBundle.load('integration_test/goldens/$_platform/$name'))
          .buffer
          .asUint8List(),
    );
    final diff = _diff(atlas, golden);
    if (diff > _maxDiffFraction) _printGolden('actual_$name', atlasPng);
    expect(
      diff,
      lessThanOrEqualTo(_maxDiffFraction),
      reason: 'fraction of differing pixels vs the $_platform golden',
    );
  });

  testWidgets('a scope taller than the screen animates where it shows', (
    tester,
  ) async {
    // A sticker pack in a list: a non-scrolling grid in a scope, scrolled so
    // that only the scope's middle shows. Its corners and centre are off
    // screen, where the engine paints nothing (ADR 0021, point 9).
    final controller = ScrollController();
    addTearDown(controller.dispose);
    final view = tester.view.physicalSize / tester.view.devicePixelRatio;
    const columns = 4;
    final cell = view.width / columns;
    // At least 4 screens tall, so that the window one screen down ends
    // above the centre.
    final rows = (view.height * 4 / cell).ceil() + 2;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ListView(
            key: const Key('tallList'),
            controller: controller,
            children: [
              StickerAtlasScope(
                key: const Key('tallScope'),
                child: GridView.count(
                  crossAxisCount: columns,
                  shrinkWrap: true,
                  physics: const NeverScrollableScrollPhysics(),
                  children: [
                    for (var i = 0; i < rows * columns; i++)
                      WebmSticker(
                        source: corpusSource(
                          playableCorpus[i % playableCorpus.length],
                        ),
                        placeholder: const ColoredBox(color: Color(0x14000000)),
                      ),
                  ],
                ),
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump(const Duration(milliseconds: 300));
    controller.jumpTo(view.height);
    final slots = find.byType(StickerAtlasSlot);
    var last = -1;
    for (var i = 0; i < 300; i++) {
      await tester.pump(const Duration(milliseconds: 50));
      final n = slots.evaluate().length;
      if (n >= columns * 2 && n == last && i > 20) break;
      last = n;
    }
    expect(slots, findsAtLeastNWidgets(columns * 2));
    final scope = tester.getRect(find.byKey(const Key('tallScope')));
    expect(scope.top, lessThan(0));
    expect(scope.center.dy, greaterThan(view.height));

    final dpr = tester.view.devicePixelRatio;
    final list = tester.getRect(find.byKey(const Key('tallList')));
    // The middle half of the list: clear of the status bar.
    final crop = Rect.fromLTRB(
      list.left * dpr,
      (list.top + list.height / 4) * dpr,
      list.right * dpr,
      (list.bottom - list.height / 4) * dpr,
    );
    await _elapse(tester, const Duration(milliseconds: 300));
    final (a, _) = await _screenshot();
    await _elapse(tester, const Duration(milliseconds: 400));
    final (b, _) = await _screenshot();
    expect(
      _diff(a, b, crop),
      greaterThan(0.001),
      reason: 'playing atlas stickers should change',
    );
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('playback advances, pauses and resumes', (tester) async {
    await tester.pumpWidget(const ExampleApp());
    await tester.tap(find.byKey(const Key('openCheckerboard')));
    await tester.pumpAndSettle();
    await _waitForTextures(tester, checkerboardStickers.length);
    expect(WebmStickers.debugActiveCount.value, greaterThan(0));

    final rect = _stickerRect(tester); // vp9_alpha_512, animated.

    Future<double> change(Duration gap) async {
      final (a, _) = await _screenshot();
      await _elapse(tester, gap);
      final (b, _) = await _screenshot();
      return _diff(a, b, rect);
    }

    expect(
      await change(const Duration(milliseconds: 400)),
      greaterThan(0.001),
      reason: 'playing sticker should change',
    );

    await tester.tap(find.byKey(const Key('play')));
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, 0);
    await _elapse(tester, const Duration(milliseconds: 300));
    expect(
      await change(const Duration(milliseconds: 400)),
      lessThanOrEqualTo(0.0001),
      reason: 'paused sticker should hold its frame',
    );

    await tester.tap(find.byKey(const Key('play')));
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, greaterThan(0));
    expect(
      await change(const Duration(milliseconds: 400)),
      greaterThan(0.001),
      reason: 'resumed sticker should change',
    );
  });

  testWidgets('pauses when offscreen or backgrounded', (tester) async {
    final controller = ScrollController();
    addTearDown(controller.dispose);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: ListView(
            controller: controller,
            // Keep the sticker out of the cache extent when scrolled away.
            scrollCacheExtent: const ScrollCacheExtent.pixels(0),
            children: [
              SizedBox(
                height: 200,
                child: WebmSticker(source: corpusSource('vp9_alpha_512.webm')),
              ),
              const SizedBox(height: 3000),
            ],
          ),
        ),
      ),
    );
    await _waitForTextures(tester, 1);
    expect(WebmStickers.debugActiveCount.value, 1);

    final rect = _stickerRect(tester);
    final (a, _) = await _screenshot();
    await _elapse(tester, const Duration(milliseconds: 400));
    final (b, _) = await _screenshot();
    expect(_diff(a, b, rect), greaterThan(0.001), reason: 'on screen: plays');

    // Scrolled away.
    controller.jumpTo(2000);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, 0, reason: 'scrolled off');

    // Back in view.
    controller.jumpTo(0);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, 1, reason: 'scrolled back');

    // Backgrounded.
    // The framework only allows real transitions, one step at a time.
    for (final s in [
      AppLifecycleState.inactive,
      AppLifecycleState.hidden,
      AppLifecycleState.paused,
    ]) {
      _binding.handleAppLifecycleStateChanged(s);
    }
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, 0, reason: 'app hidden');
    for (final s in [
      AppLifecycleState.hidden,
      AppLifecycleState.inactive,
      AppLifecycleState.resumed,
    ]) {
      _binding.handleAppLifecycleStateChanged(s);
    }
    await tester.pump(const Duration(milliseconds: 300));
    expect(WebmStickers.debugActiveCount.value, 1, reason: 'app resumed');
  });
}
