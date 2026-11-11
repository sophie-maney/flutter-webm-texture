// Measurement helpers shared by the perf integration tests
// (sched_perf_test.dart, atlas_perf_test.dart).
import 'dart:ffi';

import 'package:ffi/ffi.dart';

final class _Timeval extends Struct {
  @Int64()
  external int seconds;

  // suseconds_t is 32 bits on Darwin and 64 on Linux; both little-endian and
  // below 10^6, so the low half is the value.
  @Int32()
  external int micros;
}

typedef _GetrusageC = Int32 Function(Int32 who, Pointer<Uint8> usage);
typedef _GetrusageDart = int Function(int who, Pointer<Uint8> usage);

final _getrusage = DynamicLibrary.process()
    .lookupFunction<_GetrusageC, _GetrusageDart>('getrusage');

/// User + system CPU time of the whole process so far.
Duration cpuTime() {
  // struct rusage starts with ru_utime and ru_stime (16 bytes each on both
  // platforms); the rest is 14 longs.
  final usage = calloc<Uint8>(256);
  try {
    if (_getrusage(0 /* RUSAGE_SELF */, usage) != 0) {
      throw StateError('getrusage failed');
    }
    Duration at(int offset) {
      final t = Pointer<_Timeval>.fromAddress(usage.address + offset).ref;
      return Duration(seconds: t.seconds, microseconds: t.micros);
    }

    return at(0) + at(16);
  } finally {
    calloc.free(usage);
  }
}

/// Process CPU over [window] as a percentage of one core.
Future<double> cpuPercent(Duration window) async {
  final start = cpuTime();
  final wall = Stopwatch()..start();
  await Future<void>.delayed(window);
  final used = cpuTime() - start;
  return used.inMicroseconds / wall.elapsedMicroseconds * 100;
}

/// p50/p90/p99 (ms) and count of every complete event named [name].
Map<String, num?> durations(List<Map<String, dynamic>> events, String name) {
  final ms = <double>[];
  final open = <int, List<num>>{};
  final sorted = [...events]
    ..sort((a, b) => ((a['ts'] ?? 0) as num).compareTo((b['ts'] ?? 0) as num));
  for (final e in sorted) {
    if (e['name'] != name) continue;
    final tid = (e['tid'] ?? 0) as int;
    switch (e['ph']) {
      case 'X':
        ms.add((e['dur'] as num) / 1000);
      case 'B':
        (open[tid] ??= []).add(e['ts'] as num);
      case 'E':
        final stack = open[tid];
        if (stack != null && stack.isNotEmpty) {
          ms.add(((e['ts'] as num) - stack.removeLast()) / 1000);
        }
    }
  }
  return summary(ms);
}

/// Count and p50/p90/p99 of [ms]; the percentiles are null when it is
/// empty.
Map<String, num?> summary(List<double> ms) {
  final sorted = [...ms]..sort();
  double? pct(double p) => sorted.isEmpty
      ? null
      : sorted[((p / 100 * sorted.length).ceil() - 1).clamp(
          0,
          sorted.length - 1,
        )];
  return {'n': sorted.length, 'p50': pct(50), 'p90': pct(90), 'p99': pct(99)};
}
