// Structural checks on the native build wiring that CI can run without a
// device: the iOS symlinks cover src/, and the example bundles the
// corpus that integration_test/probe_test.dart probes.
@TestOn('vm')
library;

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

void main() {
  test('ios/.../wt_core links every src/ subdirectory', () {
    const root = 'ios/webm_texture/Sources/wt_core';
    final dirs = Directory('src')
        .listSync()
        .whereType<Directory>()
        .map((d) => d.path.split('/').last)
        .toSet();
    final links = Directory(root)
        .listSync(followLinks: false)
        .whereType<Link>()
        .map((l) => l.path.split('/').last)
        .toSet();

    expect(
      links,
      dirs,
      reason: 'add a symlink in $root for each directory under src/',
    );
    for (final d in dirs) {
      expect(Link('$root/$d').targetSync(), '../../../../src/$d');
      expect(Directory('$root/$d').existsSync(), isTrue, reason: '$d dangles');
    }
    expect(
      Link('$root/include/wt_api.h').targetSync(),
      endsWith('src/api/wt_api.h'),
    );
  });

  test('the example bundles every corpus file', () {
    final manifest = jsonDecode(
      File('test_assets/manifest.json').readAsStringSync(),
    ) as Map<String, dynamic>;
    final names = [
      for (final f in (manifest['files'] as List)) (f as Map)['name'] as String,
    ];
    final pubspec = File('example/pubspec.yaml').readAsStringSync();
    for (final name in ['manifest.json', ...names]) {
      expect(
        pubspec,
        contains('- test_assets/$name'),
        reason: 'add test_assets/$name to example/pubspec.yaml assets',
      );
    }
    expect(Link('example/test_assets').targetSync(), '../test_assets');
  });
}
