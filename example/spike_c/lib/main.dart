// Spike C (T0.9): grid density. 60 animated stickers at 30 fps, drawn either
// as 60 platform textures (MODE=textures) or as one atlas texture with 60
// clipped `Texture` children (MODE=atlas).
//
// Native code (Android: spike_c.cc + MainActivity.kt, iOS: AppDelegate.swift)
// owns one render thread. Every 1/30 s it copies the next frame of each
// sticker from a shared frame bank into the platform buffer(s) and notifies
// Flutter. Both modes do the same CPU copy work, so differences come from
// the number of textures, buffer queues and frame notifications.
//
// After WARMUP_SECS the app records Flutter `FrameTiming`s (the data DevTools
// shows in its frame chart), native tick timings, process CPU time and memory
// for RUN_SECS, then logs one SPIKE_C_RESULT JSON (in chunks) for
// tool/collect.py.

import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

const _channel = MethodChannel('spike_c');

/// Build-time switches, set with `--dart-define`.
const _mode = String.fromEnvironment('MODE', defaultValue: 'textures');
const _count = int.fromEnvironment('COUNT', defaultValue: 60);
const _atlasW = int.fromEnvironment('ATLAS_W', defaultValue: 2048);
const _atlasH = int.fromEnvironment('ATLAS_H', defaultValue: 2048);
const _runSecs = int.fromEnvironment('RUN_SECS', defaultValue: 120);
const _warmupSecs = int.fromEnvironment('WARMUP_SECS', defaultValue: 10);

/// Atlas only: transparent border around each slot (slot pitch = 128 + 2g).
const _gutter = int.fromEnvironment('GUTTER');
const _label = String.fromEnvironment('LABEL', defaultValue: 'unlabelled');

/// Sticker geometry: square, physical texels.
const int stickerSize = 128;
const int fps = 30;
const int bankFrames = 30;

/// Number of drift windows the run is split into.
const int windows = 4;

int _pm(double c, double a) => (c * a).round().clamp(0, 255);

/// A 1-second loop of a synthetic sticker, premultiplied, tightly packed.
/// A soft-edged disc orbits the centre while its hue cycles; the top 8 rows
/// show the frame index in binary so screenshots prove frames advance.
/// [bgra] swaps R and B for iOS.
Uint8List buildBank({required bool bgra}) {
  const s = stickerSize;
  final out = Uint8List(bankFrames * s * s * 4);
  for (var f = 0; f < bankFrames; f++) {
    final t = f / bankFrames * 2 * math.pi;
    final cx = s / 2 + math.cos(t) * s * 0.18;
    final cy = s / 2 + math.sin(t) * s * 0.18;
    final r = s * 0.30;
    // Hue → RGB (simple HSV with s = v = 1).
    final h = f / bankFrames * 6;
    final x = 1 - ((h % 2) - 1).abs();
    final rgb = switch (h.floor()) {
      0 => [1.0, x, 0.0],
      1 => [x, 1.0, 0.0],
      2 => [0.0, 1.0, x],
      3 => [0.0, x, 1.0],
      4 => [x, 0.0, 1.0],
      _ => [1.0, 0.0, x],
    };
    final base = f * s * s * 4;
    for (var py = 0; py < s; py++) {
      for (var px = 0; px < s; px++) {
        double a, cr, cg, cb;
        if (py < 8) {
          final bit = 4 - px * 5 ~/ s;
          final on = (f >> bit) & 1 == 1;
          a = 1;
          cr = cg = cb = on ? 1 : 0;
        } else {
          final d = math.sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
          // 6-px soft edge, plus a faint translucent halo out to r + 20.
          final core = ((r - d) / 6 + 0.5).clamp(0.0, 1.0);
          final halo = d < r + 20 ? 0.25 * (1 - (d - r).clamp(0, 20) / 20) : 0;
          a = math.max(core, halo.toDouble());
          cr = rgb[0];
          cg = rgb[1];
          cb = rgb[2];
        }
        final o = base + (py * s + px) * 4;
        final r8 = _pm(cr * 255, a),
            g8 = _pm(cg * 255, a),
            b8 = _pm(cb * 255, a);
        out[o] = bgra ? b8 : r8;
        out[o + 1] = g8;
        out[o + 2] = bgra ? r8 : b8;
        out[o + 3] = (a * 255).round();
      }
    }
  }
  return out;
}

/// Slot origin of sticker [i] in the atlas (row-major, 128-px slots).
Offset slotOf(int i) {
  const pitch = stickerSize + 2 * _gutter;
  const perRow = _atlasW ~/ pitch;
  return Offset(
    (i % perRow * pitch + _gutter).toDouble(),
    (i ~/ perRow * pitch + _gutter).toDouble(),
  );
}

/// Nearest-rank percentile of [xs] (not sorted in place).
double pct(List<double> xs, double p) {
  if (xs.isEmpty) return double.nan;
  final s = [...xs]..sort();
  final k = (p / 100 * s.length).ceil().clamp(1, s.length) - 1;
  return s[k];
}

Map<String, Object> summarise(List<double> xs) => {
  'n': xs.length,
  'p50': _r(pct(xs, 50)),
  'p90': _r(pct(xs, 90)),
  'p99': _r(pct(xs, 99)),
  'max': _r(xs.isEmpty ? double.nan : xs.reduce(math.max)),
  'mean': _r(xs.isEmpty ? double.nan : xs.reduce((a, b) => a + b) / xs.length),
};

double _r(double v) => v.isNaN ? -1 : (v * 1000).round() / 1000;

void main() => runApp(const SpikeApp());

class SpikeApp extends StatelessWidget {
  const SpikeApp({super.key});

  @override
  Widget build(BuildContext context) =>
      const MaterialApp(debugShowCheckedModeBanner: false, home: SpikePage());
}

class SpikePage extends StatefulWidget {
  const SpikePage({super.key});

  @override
  State<SpikePage> createState() => _SpikePageState();
}

class _Frame {
  _Frame(this.t, this.build, this.raster, this.total);
  final double t; // seconds since measurement start (raster finish)
  final double build, raster, total; // ms
}

class _SpikePageState extends State<SpikePage> {
  List<int> _ids = const [];
  Map<Object?, Object?> _info = const {};
  String _status = 'starting';
  String? _error;

  final _frames = <_Frame>[];
  final _samples = <Map<Object?, Object?>>[];
  bool _measuring = false;
  final _clock = Stopwatch();
  Timer? _sampler;

  @override
  void initState() {
    super.initState();
    _init();
  }

  Future<void> _init() async {
    try {
      final info = await _channel.invokeMapMethod<Object?, Object?>('info');
      final isIos = info?['platform'] == 'ios';
      final bank = buildBank(bgra: isIos);
      final res = await _channel.invokeMapMethod<Object?, Object?>('start', {
        'mode': _mode,
        'count': _count,
        'size': stickerSize,
        'atlasW': _atlasW,
        'atlasH': _atlasH,
        'fps': fps,
        'gutter': _gutter,
        'frames': bankFrames,
        'bank': bank,
      });
      setState(() {
        _info = {...?info, ...?res};
        _ids = (res!['textureIds']! as List).cast<int>();
        _status = 'warming up ${_warmupSecs}s';
      });
      debugPrint('SPIKE_C_STARTED $_label ${jsonEncode(_info)}');
      await Future<void>.delayed(const Duration(seconds: _warmupSecs));
      await _measure();
    } catch (e, st) {
      setState(() => _error = '$e');
      debugPrint('SPIKE_C_ERROR $e\n$st');
    }
  }

  void _onTimings(List<FrameTiming> ts) {
    if (!_measuring) return;
    final now = _clock.elapsedMicroseconds / 1e6;
    for (final t in ts) {
      _frames.add(
        _Frame(
          now,
          t.buildDuration.inMicroseconds / 1000,
          t.rasterDuration.inMicroseconds / 1000,
          t.totalSpan.inMicroseconds / 1000,
        ),
      );
    }
  }

  Future<void> _sample() async {
    final s = await _channel.invokeMapMethod<Object?, Object?>('sample');
    if (s != null) _samples.add({...s, 't': _clock.elapsedMilliseconds / 1000});
  }

  Future<void> _measure() async {
    // The status line is rebuilt only before and after the window, so the
    // UI thread does no work of its own while measuring.
    setState(() => _status = 'measuring ${_runSecs}s');
    await Future<void>.delayed(const Duration(milliseconds: 500));
    await _channel.invokeMethod<void>('resetStats');
    SchedulerBinding.instance.addTimingsCallback(_onTimings);
    _clock.start();
    _measuring = true;
    await _sample();
    _sampler = Timer.periodic(const Duration(seconds: 5), (_) => _sample());
    await Future<void>.delayed(const Duration(seconds: _runSecs));
    _measuring = false;
    _sampler?.cancel();
    await _sample();
    SchedulerBinding.instance.removeTimingsCallback(_onTimings);
    final native = await _channel.invokeMapMethod<Object?, Object?>('stats');
    final result = _result(native ?? const {});
    final text = jsonEncode(result);
    // Log in chunks: logcat and os_log truncate long lines.
    const chunk = 700;
    final n = (text.length / chunk).ceil();
    for (var i = 0; i < n; i++) {
      final part = text.substring(
        i * chunk,
        math.min(text.length, (i + 1) * chunk),
      );
      debugPrint('SPIKE_C_RESULT ${i + 1}/$n $part');
    }
    setState(() {
      final raster = (result['frames']! as Map)['raster'] as Map;
      _status =
          'done: ${(result['frames']! as Map)['fps']} fps, '
          'raster p90 ${raster['p90']} p99 ${raster['p99']} ms';
    });
  }

  Map<String, Object> _result(Map<Object?, Object?> native) {
    List<double> col(Iterable<_Frame> fs, double Function(_Frame) f) =>
        fs.map(f).toList();
    final secs = _runSecs.toDouble();
    final perWindow = <Map<String, Object>>[];
    for (var w = 0; w < windows; w++) {
      final lo = secs * w / windows, hi = secs * (w + 1) / windows;
      final fs = _frames.where((f) => f.t >= lo && f.t < hi).toList();
      final ss = _samples.where((s) {
        final t = (s['t']! as num).toDouble();
        return t >= lo && t <= hi;
      }).toList();
      double? cpuRate;
      if (ss.length >= 2) {
        final c0 = (ss.first['cpuMs']! as num).toDouble();
        final c1 = (ss.last['cpuMs']! as num).toDouble();
        final t0 = (ss.first['t']! as num).toDouble();
        final t1 = (ss.last['t']! as num).toDouble();
        cpuRate = t1 > t0 ? (c1 - c0) / (t1 - t0) / 10 : null; // % of 1 core
      }
      perWindow.add({
        'from': lo,
        'to': hi,
        'frames': fs.length,
        'rasterP90': _r(pct(col(fs, (f) => f.raster), 90)),
        'rasterP99': _r(pct(col(fs, (f) => f.raster), 99)),
        'buildP90': _r(pct(col(fs, (f) => f.build), 90)),
        'cpuPct': cpuRate == null ? -1 : _r(cpuRate),
      });
    }
    final first = _samples.first, last = _samples.last;
    final dt = (last['t']! as num).toDouble() - (first['t']! as num).toDouble();
    final cpu =
        ((last['cpuMs']! as num) - (first['cpuMs']! as num)).toDouble() / dt;
    final ticks = native['tickWorkMs'] as List? ?? const [];
    final late = native['tickLateMs'] as List? ?? const [];
    return {
      'label': _label,
      'mode': _mode,
      'count': _count,
      'atlas': _mode == 'atlas' ? '${_atlasW}x$_atlasH' : '-',
      'gutter': _gutter,
      'runSecs': _runSecs,
      'kDebugMode': kDebugMode,
      'kProfileMode': kProfileMode,
      'info': _info.map((k, v) => MapEntry('$k', v is List ? v.length : v)),
      'frames': {
        'fps': _r(_frames.length / secs),
        'build': summarise(col(_frames, (f) => f.build)),
        'raster': summarise(col(_frames, (f) => f.raster)),
        'total': summarise(col(_frames, (f) => f.total)),
        'over16ms': _frames.where((f) => f.raster > 16.67).length,
        'over33ms': _frames.where((f) => f.raster > 33.3).length,
      },
      'native': {
        'ticks': ticks.length,
        'expectedTicks': (secs * fps).round(),
        'work': summarise([for (final t in ticks) (t as num).toDouble()]),
        'late': summarise([for (final t in late) (t as num).toDouble()]),
        'errors': native['errors'] ?? -1,
        'notifies': native['notifies'] ?? -1,
        'extra': native['extra'] ?? const {},
      },
      'cpuPctOfOneCore': _r(cpu / 10),
      'memory': {
        'first': first,
        'last': last,
        'max': {
          for (final k in last.keys.where((k) => k != 't'))
            '$k': _samples
                .map((s) => (s[k] as num? ?? 0).toDouble())
                .reduce(math.max),
        },
      },
      'windows': perWindow,
    };
  }

  @override
  Widget build(BuildContext context) {
    final mq = MediaQuery.of(context);
    final width = mq.size.width;
    final height = mq.size.height - mq.padding.vertical - 48;
    // Fewest columns such that all stickers fit on screen.
    var cols = 4;
    // Bounded: the first frame can report a zero-sized view.
    while (cols < _count) {
      final tile = width / cols;
      if ((_count / cols).ceil() * tile <= height) break;
      cols++;
    }
    final tile = (width / cols).floorToDouble();
    return Scaffold(
      backgroundColor: const Color(0xFFE8EAED),
      body: SafeArea(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            SizedBox(
              height: 48,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Text(
                  _error ??
                      '$_label · $_mode · $_count × ${stickerSize}px · $_status',
                  style: const TextStyle(fontSize: 12),
                  maxLines: 2,
                ),
              ),
            ),
            if (_ids.isNotEmpty)
              Wrap(
                children: [
                  for (var i = 0; i < _count; i++)
                    SizedBox.square(
                      dimension: tile,
                      child: Padding(
                        padding: const EdgeInsets.all(2),
                        child: _mode == 'atlas'
                            ? AtlasSlot(
                                textureId: _ids.single,
                                slot: slotOf(i),
                                atlasSize: const Size(
                                  _atlasW + 0.0,
                                  _atlasH + 0.0,
                                ),
                              )
                            : Texture(textureId: _ids[i]),
                      ),
                    ),
                ],
              ),
          ],
        ),
      ),
    );
  }
}

/// One sticker drawn from the atlas: the whole atlas texture, scaled so a
/// 128-px slot fills this box, translated so [slot] is at the origin, and
/// clipped to the box. This is the T4.3 `StickerAtlasScope` child shape.
class AtlasSlot extends StatelessWidget {
  const AtlasSlot({
    super.key,
    required this.textureId,
    required this.slot,
    required this.atlasSize,
  });

  final int textureId;
  final Offset slot;
  final Size atlasSize;

  @override
  Widget build(BuildContext context) => LayoutBuilder(
    builder: (context, c) {
      final scale = c.maxWidth / stickerSize;
      return ClipRect(
        child: OverflowBox(
          alignment: Alignment.topLeft,
          minWidth: 0,
          minHeight: 0,
          maxWidth: double.infinity,
          maxHeight: double.infinity,
          child: Transform.translate(
            offset: -slot * scale,
            child: SizedBox(
              width: atlasSize.width * scale,
              height: atlasSize.height * scale,
              child: Texture(textureId: textureId),
            ),
          ),
        ),
      );
    },
  );
}
