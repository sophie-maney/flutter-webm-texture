// Spike C: record the engine timeline (the data DevTools shows) through the
// VM service and summarise raster-thread event durations.
//
// usage: dart tool/timeline.dart <vm-service-http-uri> <seconds> [label]
//
// Needed on iOS: `textureFrameAvailable` re-rasterises the last layer tree
// without reporting a FrameTiming, so the in-app numbers stay empty there.
// Fetches and clears the timeline every 10 s so the ring buffer never wraps.
// Prints one JSON object. dart:io only.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

Future<void> main(List<String> args) async {
  final http = Uri.parse(args[0]);
  final seconds = int.parse(args[1]);
  final ws = http.replace(
    scheme: 'ws',
    path: '${http.path.endsWith('/') ? http.path : '${http.path}/'}ws',
  );
  final socket = await WebSocket.connect(ws.toString());
  final pending = <int, Completer<Map<String, dynamic>>>{};
  var nextId = 0;
  socket.listen((msg) {
    final m = jsonDecode(msg as String) as Map<String, dynamic>;
    final id = m['id'];
    if (id is String) pending.remove(int.parse(id))?.complete(m);
  });
  Future<Map<String, dynamic>> call(String method, [Map<String, Object>? p]) {
    final id = nextId++;
    final c = pending[id] = Completer();
    socket.add(
      jsonEncode({
        'jsonrpc': '2.0',
        'id': '$id',
        'method': method,
        'params': p ?? {},
      }),
    );
    return c.future;
  }

  await call('setVMTimelineFlags', {
    'recordedStreams': ['Embedder', 'GC', 'Dart'],
  });
  await call('clearVMTimeline');

  // name -> durations (ms); per-thread stacks for B/E pairs.
  final durs = <String, List<double>>{};
  final threadNames = <int, String>{};
  void addEvents(List<dynamic> events) {
    final stacks = <int, List<Map<String, dynamic>>>{};
    events.sort((a, b) => ((a['ts'] ?? 0) as num).compareTo(b['ts'] ?? 0));
    for (final e in events.cast<Map<String, dynamic>>()) {
      final tid = (e['tid'] ?? 0) as int;
      final ph = e['ph'];
      if (ph == 'M' && e['name'] == 'thread_name') {
        threadNames[tid] = '${e['args']?['name']}';
      } else if (ph == 'X') {
        (durs[e['name'] as String] ??= []).add((e['dur'] as num) / 1000);
      } else if (ph == 'B') {
        (stacks[tid] ??= []).add(e);
      } else if (ph == 'E') {
        final s = stacks[tid];
        if (s == null || s.isEmpty) continue;
        final b = s.removeLast();
        final d = ((e['ts'] as num) - (b['ts'] as num)) / 1000;
        (durs[b['name'] as String] ??= []).add(d);
      }
    }
  }

  final end = DateTime.now().add(Duration(seconds: seconds));
  while (DateTime.now().isBefore(end)) {
    final left = end.difference(DateTime.now());
    await Future<void>.delayed(
      left < const Duration(seconds: 10) ? left : const Duration(seconds: 10),
    );
    final r = await call('getVMTimeline');
    await call('clearVMTimeline');
    addEvents((r['result']?['traceEvents'] as List?) ?? const []);
  }
  await socket.close();

  double pct(List<double> xs, double p) {
    final s = [...xs]..sort();
    final k = ((p / 100 * s.length).ceil()).clamp(1, s.length) - 1;
    return (s[k] * 1000).round() / 1000;
  }

  final out = <String, Object>{};
  final keys =
      durs.keys
          .where(
            (k) =>
                k.contains('Rasterizer') ||
                k.contains('Animator') ||
                k.contains('Frame') ||
                k.contains('CopyPixelBuffer') ||
                k.contains('Texture') ||
                k.contains('Draw'),
          )
          .toList()
        ..sort();
  for (final k in keys) {
    final xs = durs[k]!;
    if (xs.length < 10) continue;
    out[k] = {
      'n': xs.length,
      'perSec': (xs.length / seconds * 100).round() / 100,
      'p50': pct(xs, 50),
      'p90': pct(xs, 90),
      'p99': pct(xs, 99),
    };
  }
  stdout.writeln(
    jsonEncode({
      'label': args.length > 2 ? args[2] : '',
      'seconds': seconds,
      'events': out,
    }),
  );
  exit(0);
}
