// Rebuilds golden PNGs from `GOLDEN <platform>/<name> <i>/<n> <base64>` lines
// on stdin (printed by example/integration_test/app_test.dart when run with
// `--dart-define=UPDATE_GOLDENS=true`, or when a golden check fails) and writes
// them under example/integration_test/goldens/.
//
//   cd example && fvm flutter test integration_test/app_test.dart -d <device> \
//     --dart-define=UPDATE_GOLDENS=true | fvm dart run ../tool/extract_goldens.dart
import 'dart:convert';
import 'dart:io';

void main() {
  final chunks = <String, Map<int, String>>{};
  final totals = <String, int>{};
  final line = RegExp(r'GOLDEN (\S+) (\d+)/(\d+) (\S+)');
  String? raw;
  while ((raw = stdin.readLineSync()) != null) {
    final m = line.firstMatch(raw!);
    if (m == null) continue;
    final name = m[1]!;
    chunks.putIfAbsent(name, () => {})[int.parse(m[2]!)] = m[4]!;
    totals[name] = int.parse(m[3]!);
  }
  final root = Directory('integration_test/goldens');
  if (!root.existsSync()) {
    stderr.writeln('run from example/ (integration_test/goldens not found)');
    exit(2);
  }
  var failed = false;
  for (final entry in chunks.entries) {
    final total = totals[entry.key]!;
    if (entry.value.length != total) {
      stderr.writeln('${entry.key}: got ${entry.value.length}/$total chunks');
      failed = true;
      continue;
    }
    final b64 = [for (var i = 1; i <= total; i++) entry.value[i]!].join();
    final file = File('${root.path}/${entry.key}')
      ..createSync(recursive: true)
      ..writeAsBytesSync(base64Decode(b64));
    stdout.writeln('wrote ${file.path}');
  }
  if (chunks.isEmpty) {
    stderr.writeln('no GOLDEN lines on stdin');
    failed = true;
  }
  if (failed) exit(1);
}
