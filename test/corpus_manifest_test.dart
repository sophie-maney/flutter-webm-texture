// Structural checks on the committed test corpus (test_assets/), so CI catches
// a stale manifest or an oversized corpus without needing ffmpeg. The full
// check, which re-probes every clip, is `tool/gen_corpus.sh --check`.
@TestOn('vm')
library;

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

/// The committed corpus must stay under 5 MB (see EXECUTION_PLAN.md, T0.3).
const _maxCorpusBytes = 5000000;

void main() {
  final assets = Directory('test_assets');
  final manifest = jsonDecode(
    File('test_assets/manifest.json').readAsStringSync(),
  ) as Map<String, dynamic>;
  final files = (manifest['files'] as List).cast<Map<String, dynamic>>();

  String refPath(String name, int frame) =>
      'ref/$name/${frame.toString().padLeft(3, '0')}.rgba.gz';

  test('every listed file exists with the listed size', () {
    for (final f in files) {
      final file = File('test_assets/${f['name']}');
      expect(file.existsSync(), isTrue, reason: '${f['name']} is missing');
      expect(file.lengthSync(), f['bytes'], reason: '${f['name']} size');
    }
  });

  test('frame metadata is self-consistent', () {
    for (final f in files.where((f) => f['expect'] != 'invalid')) {
      final n = f['frame_count'] as int;
      final name = f['name'];
      expect((f['pts_ms'] as List).length, n, reason: '$name pts_ms');
      expect((f['durations_ms'] as List).length, n, reason: '$name durations');
      expect(
        (f['durations_ms'] as List).every((d) => (d as int) > 0),
        isTrue,
        reason: '$name has a non-positive duration',
      );
      for (final i in (f['ref_frames'] as List).cast<int>()) {
        expect(i, inInclusiveRange(0, n - 1), reason: '$name ref frame $i');
      }
    }
  });

  test('reference frames are width*height*4 bytes of premultiplied RGBA', () {
    for (final f in files) {
      final frames = (f['ref_frames'] as List).cast<int>();
      for (final i in frames) {
        final path = refPath(f['name'] as String, i);
        final file = File('test_assets/$path');
        expect(file.existsSync(), isTrue, reason: '$path is missing');
        final rgba = gzip.decode(file.readAsBytesSync());
        expect(
          rgba.length,
          (f['width'] as int) * (f['height'] as int) * 4,
          reason: '$path size',
        );
        for (var p = 0; p < rgba.length; p += 4) {
          final a = rgba[p + 3];
          if (rgba[p] > a || rgba[p + 1] > a || rgba[p + 2] > a) {
            fail('$path pixel ${p ~/ 4} is not premultiplied');
          }
        }
      }
    }
  });

  test('test_assets holds only listed files and fits the size budget', () {
    final listed = <String>{'manifest.json', 'README.md'};
    for (final f in files) {
      listed.add(f['name'] as String);
      for (final i in (f['ref_frames'] as List).cast<int>()) {
        listed.add(refPath(f['name'] as String, i));
      }
    }
    var total = 0;
    final unlisted = <String>[];
    for (final e in assets.listSync(recursive: true).whereType<File>()) {
      final rel = e.path
          .substring(assets.path.length + 1)
          .replaceAll(Platform.pathSeparator, '/');
      if (rel.endsWith('.DS_Store')) continue;
      total += e.lengthSync();
      if (!listed.contains(rel)) unlisted.add(rel);
    }
    expect(unlisted, isEmpty, reason: 'files missing from manifest.json');
    expect(total, lessThan(_maxCorpusBytes));
  });
}
