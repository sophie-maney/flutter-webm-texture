// T4.3: StickerAtlasScope's acceptance numbers on a device or simulator.
//
// 60 visible stickers in atlas mode (the Stress page as a 6 x 10 grid in a
// StickerAtlasScope, every cell its own animation) play while this test
// measures, over [_window]:
//
// - Content rate: the fastest clip on screen (30 fps; the corpus also has a
//   variable-rate clip at 40 ms and more per frame).
// - Presents per second of each atlas page (`atlasStats`): each is one
//   platform frame for Flutter. They must be at the content rate: every tick
//   delivered (no fewer) and no extra frames (no more) (ADR 0021).
// - Flutter frames per second. Android: the engine runs a full frame for
//   each page image, counted by a persistent frame callback. The binding's
//   frame policy is `benchmark` meanwhile: its live policies schedule the
//   next frame after every frame, which an app doesn't, while `benchmark`
//   still runs every frame the engine asks for. iOS: `textureFrameAvailable` re-rasterises without a Dart
//   frame and reports no FrameTiming (spike C), so it counts
//   `Rasterizer::DrawToSurfaces` in the engine timeline, whose p90 must be
//   within [_rasterBudgetMs].
// - The high tier (60 fps cap), so the 30-fps clips play at their own rate
//   on emulators that detect as low tier (15 fps).
// - The whole app's CPU (getrusage), and the pages' memory.
//
//   cd example && fvm flutter test integration_test/atlas_perf_test.dart -d <device>
//
// On Android add `--no-dds` (see sched_perf_test.dart): the app records the
// timeline through its own VM service.
//
// Run the Android emulator with the Mac's screen unlocked: while it is
// locked the emulator hands frames back slowly and this measures ~3 fps
// (ADR 0021).
//
// Prints one `ATLAS_PERF {json}` line. Not part of all_test.dart: the
// numbers depend on the machine.
// ignore_for_file: implementation_imports
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture/src/sticker_atlas_scope.dart';
import 'package:webm_texture/webm_texture.dart';
import 'package:webm_texture_example/stress_page.dart';

import 'perf_util.dart';

/// The grid: 6 x 10 cells fill the screen, so exactly 60 stickers show.
const _columns = 6;
const _rows = 10;
const _visible = _columns * _rows;

/// The fastest clip on the Stress page plays at 30 fps.
const _contentFps = 30.0;

/// Flutter frames and page presents may be off the content rate by this
/// fraction: timer slack, and the 33/34 ms frames of 30-fps WebM.
const _rateTolerance = 0.1;

/// iOS simulator raster p90 budget (EXECUTION_PLAN.md, T4.3; spike C
/// measured 2.0 ms).
const _rasterBudgetMs = 3.0;

const _warmUp = Duration(seconds: 8);
const _window = Duration(seconds: 20);

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('60 visible stickers in atlas mode: frame rate and raster', (
    tester,
  ) async {
    await WebmStickers.configure(const StickerConfig(tier: StickerTier.high));
    await tester.pumpWidget(
      const MaterialApp(
        home: StressPage(
          initiallyPrivate: true,
          initiallyAtlas: true,
          columns: _columns,
          rows: _rows,
        ),
      ),
    );
    // Assets load, every visible cell gets its animation and slot and
    // starts playing.
    for (var i = 0; i < 600; i++) {
      await tester.pump(const Duration(milliseconds: 50));
      if (WebmStickers.debugActiveCount.value >= _visible &&
          find.byType(StickerAtlasSlot).evaluate().length >= _visible) {
        break;
      }
    }
    await tester.pump(const Duration(milliseconds: 500));
    expect(WebmStickers.debugActiveCount.value, _visible);
    expect(find.byType(StickerAtlasSlot), findsNWidgets(_visible));

    final atlas = stickerAtlasOf(
      tester.element(find.byType(WebmSticker).first),
    )!;
    final atlasId = atlas.debugAtlasId!;
    final api = WebmTextureHostApi();

    // The first loop decodes and writes each cache file; later ones read it.
    await Future<void>.delayed(_warmUp);

    // From here on only the engine asks for frames: the page textures.
    binding.framePolicy = LiveTestWidgetsFlutterBindingFramePolicy.benchmark;
    var dartFrames = 0;
    void onFrame(Duration _) => dartFrames++;
    var persistent = true;
    SchedulerBinding.instance.addPersistentFrameCallback((d) {
      if (persistent) onFrame(d);
    });
    final frameRaster = <double>[];
    void onTimings(List<FrameTiming> timings) => frameRaster.addAll([
      for (final t in timings) t.rasterDuration.inMicroseconds / 1000,
    ]);
    SchedulerBinding.instance.addTimingsCallback(onTimings);

    final before = await api.atlasStats(atlasId);
    final cpuStart = cpuTime();
    final wall = Stopwatch()..start();
    final timeline = await binding.traceTimeline(
      () => Future<void>.delayed(_window),
      streams: const ['Embedder', 'GC', 'Dart'],
    );
    wall.stop();
    final cpu =
        (cpuTime() - cpuStart).inMicroseconds / wall.elapsedMicroseconds * 100;
    final after = await api.atlasStats(atlasId);
    persistent = false;
    SchedulerBinding.instance.removeTimingsCallback(onTimings);
    binding.framePolicy = LiveTestWidgetsFlutterBindingFramePolicy.fadePointers;

    final seconds = wall.elapsedMicroseconds / 1e6;
    final events = <Map<String, dynamic>>[
      for (final e in timeline.traceEvents!) e.json!,
    ];
    final raster = durations(events, 'Rasterizer::DrawToSurfaces');
    final rasterPerSec = raster['n']! / seconds;
    final dartFps = dartFrames / seconds;
    final pages = [
      for (final p in after.pages)
        {
          'page': p.pageId,
          'size': '${p.width}x${p.height}',
          'slots': p.liveSlots,
          'buffers': p.buffers,
          'bufferMB': p.bufferBytes / (1 << 20),
          'cpuMB': p.cpuBytes / (1 << 20),
          'presentsPerSec':
              (p.presents -
                  (before.pages
                          .where((b) => b.pageId == p.pageId)
                          .firstOrNull
                          ?.presents ??
                      0)) /
              seconds,
          'presentFailures': p.presentFailures,
        },
    ];
    // iOS: texture-only frames, seen in the timeline. Android: one Dart frame
    // per engine frame.
    final flutterFps = Platform.isIOS ? rasterPerSec : dartFps;
    final result = {
      'platform': Platform.operatingSystem,
      'tier': WebmStickers.debugTier.value.name,
      'visible': _visible,
      'contentFps': _contentFps,
      'flutterFps': flutterFps,
      'dartFramesPerSec': dartFps,
      'rasterPerSec': rasterPerSec,
      'rasterMs': raster,
      'frameTimingRasterMs': summary(frameRaster),
      'pages': pages,
      // Batched attaches while the grid filled: animations per call, and
      // round trips (the platform thread waits for frames in flight).
      'attachCalls': atlas.debugAttaches.length,
      'attachSizes': [for (final (n, _) in atlas.debugAttaches) n],
      'attachMs': summary([for (final (_, ms) in atlas.debugAttaches) ms]),
      'attachMaxMs': atlas.debugAttaches.fold<double>(
        0,
        (m, a) => a.$2 > m ? a.$2 : m,
      ),
      'pageTextures': after.textures,
      'pluginTextures': after.pluginTextures,
      'cpuPercent': cpu,
    };
    binding.reportData = {...?binding.reportData, 'atlas_perf': result};
    // ignore: avoid_print
    print('ATLAS_PERF ${jsonEncode(result)}');

    const low = _contentFps * (1 - _rateTolerance);
    const high = _contentFps * (1 + _rateTolerance);
    for (final p in pages) {
      final rate = p['presentsPerSec']! as double;
      expect(rate, greaterThanOrEqualTo(low), reason: 'every tick: $p');
      expect(rate, lessThanOrEqualTo(high), reason: 'no extra presents: $p');
    }
    expect(
      flutterFps,
      lessThanOrEqualTo(high),
      reason: 'Flutter renders no extra frames',
    );
    expect(
      flutterFps,
      greaterThanOrEqualTo(low),
      reason: 'every tick reaches the screen',
    );
    if (Platform.isIOS) {
      expect(raster['p90'], lessThanOrEqualTo(_rasterBudgetMs));
    }
  });
}
