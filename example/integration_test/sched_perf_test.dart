// T3.3: the scheduler's acceptance numbers on a device or simulator.
//
// - Raster: 60 visible standalone stickers (the Stress page as a 6 x 10
//   grid, every cell its own animation) play while the engine timeline is
//   recorded; the p90 of `Rasterizer::DrawToSurfaces` must stay within
//   [_rasterBudgetMs] on iOS. iOS texture-only frames report no FrameTiming
//   (spike C), hence the timeline. Android only reports: the emulator's
//   gralloc is too slow for a standalone budget (spike C report), and the
//   raster figures are often empty there (see the end of the test).
// - CPU: the whole app's CPU time (getrusage) with no stickers at all (the
//   baseline of a debug app sitting still), while every sticker plays, and
//   while all are paused (TickerMode off), when it must be about the
//   baseline.
//
//   cd example && fvm flutter test integration_test/sched_perf_test.dart -d <device>
//
// On Android add `--no-dds`: with DDS the app can't reach its own VM service
// to record the timeline. flutter_tools then reports a loading error
// (`streamListen: ... VmServiceProxyGoldenFileComparator`) next to the test's
// own result; it comes from the golden-file listener, not from this test.
//
// Prints one `SCHED_PERF {json}` line. Not part of all_test.dart: the
// numbers depend on the machine. Real-device budgets come from T3.5.
import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:webm_texture/webm_texture.dart';
import 'package:webm_texture_example/stress_page.dart';

import 'perf_util.dart';

/// The grid: 6 x 10 cells fill the screen, so exactly 60 stickers show.
const _columns = 6;
const _rows = 10;
const _visible = _columns * _rows;

/// iOS simulator raster p90 budget (EXECUTION_PLAN.md, T3.3; spike C
/// measured 5.9 ms for 60 textures).
const _rasterBudgetMs = 8.0;

/// Paused, the app may use at most this share of one core: what a debug
/// app with no stickers at all uses (0.3–2.5 % measured), not the 35–40 %
/// of playing. The scheduler's own threads use none (ADR 0017).
const _maxPausedCpuPercent = 3.0;

const _warmUp = Duration(seconds: 8);
const _traceFor = Duration(seconds: 20);
const _cpuWindow = Duration(seconds: 5);

void main() {
  final binding = IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('60 visible standalone stickers: raster and CPU', (tester) async {
    await tester.pumpWidget(const SizedBox());
    await Future<void>.delayed(const Duration(seconds: 2));
    final baselineCpu = await cpuPercent(_cpuWindow);

    final ticking = ValueNotifier(true);
    await tester.pumpWidget(
      MaterialApp(
        home: ValueListenableBuilder<bool>(
          valueListenable: ticking,
          builder: (_, on, child) => TickerMode(enabled: on, child: child!),
          child: const StressPage(
            initiallyPrivate: true,
            initiallyAtlas: false,
            columns: _columns,
            rows: _rows,
          ),
        ),
      ),
    );
    // Assets load, every visible cell gets its animation and starts playing.
    for (var i = 0; i < 600; i++) {
      await tester.pump(const Duration(milliseconds: 50));
      if (WebmStickers.debugActiveCount.value >= _visible) break;
    }
    // Let stragglers at the edges settle.
    await tester.pump(const Duration(milliseconds: 500));
    final visible = WebmStickers.debugActiveCount.value;
    expect(visible, _visible);

    // The first loop decodes and writes each cache file; later ones read it.
    await Future<void>.delayed(_warmUp);
    final playingCpu = await cpuPercent(_cpuWindow);

    // Android reports a FrameTiming per rasterised frame; iOS doesn't for
    // texture-only frames, so it relies on the timeline.
    final frameRaster = <double>[];
    void onTimings(List<FrameTiming> timings) => frameRaster.addAll([
      for (final t in timings) t.rasterDuration.inMicroseconds / 1000,
    ]);
    SchedulerBinding.instance.addTimingsCallback(onTimings);
    final timeline = await binding.traceTimeline(
      () => Future<void>.delayed(_traceFor),
      streams: const ['Embedder', 'GC', 'Dart'],
    );
    SchedulerBinding.instance.removeTimingsCallback(onTimings);
    final events = <Map<String, dynamic>>[
      for (final e in timeline.traceEvents!) e.json!,
    ];
    final raster = durations(events, 'Rasterizer::DrawToSurfaces');
    final rasterPerSec = raster['n']! / _traceFor.inSeconds;
    final frameTimings = summary(frameRaster);

    // Pause everything; the clock and workers should go to sleep.
    ticking.value = false;
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(WebmStickers.debugActiveCount.value, 0);
    await Future<void>.delayed(const Duration(seconds: 2));
    final pausedCpu = await cpuPercent(_cpuWindow);

    final result = {
      'platform': Platform.operatingSystem,
      'visible': visible,
      'rasterMs': raster,
      'rasterPerSec': rasterPerSec,
      'frameTimingRasterMs': frameTimings,
      'baselineCpuPercent': baselineCpu,
      'playingCpuPercent': playingCpu,
      'pausedCpuPercent': pausedCpu,
    };
    binding.reportData = {...?binding.reportData, 'sched_perf': result};
    // ignore: avoid_print
    print('SCHED_PERF ${jsonEncode(result)}');

    if (Platform.isIOS) {
      expect(raster['n'], greaterThan(0), reason: 'no raster events recorded');
      expect(raster['p90'], lessThanOrEqualTo(_rasterBudgetMs));
    }
    // Android: report only. Its engine traces to systrace, not the Dart
    // timeline, and the test binding gets no FrameTimings, so both raster
    // figures may be empty; T3.5 measures real devices with `flutter drive`.
    expect(pausedCpu, lessThan(_maxPausedCpuPercent));
    ticking.dispose();
  });
}
