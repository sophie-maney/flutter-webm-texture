// Tests for tool/fetch_prebuilts.dart: the checksum maths, the release tag, and
// the fail-closed behaviour, run end to end against a fake release directory
// served through file://.
@TestOn('vm')
library;

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import '../tool/fetch_prebuilts.dart';

/// Runs the tool and returns its exit code.
Future<int> _run(List<String> args) => run(args, environment: const {});

Future<void> _sh(String exe, List<String> args, {String? cwd}) async {
  final r = await Process.run(exe, args, workingDirectory: cwd);
  if (r.exitCode != 0) fail('$exe ${args.join(' ')} failed: ${r.stderr}');
}

void main() {
  group('sha256Hex', () {
    test('matches the FIPS 180-4 vectors', () {
      expect(
        sha256Hex(const []),
        'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
      );
      expect(
        sha256Hex(utf8.encode('abc')),
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
      );
      expect(
        sha256Hex(
          utf8.encode(
            'abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
          ),
        ),
        '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1',
      );
    });

    test('handles padding boundaries and long input', () {
      // 55/56/64 bytes straddle the one-block / two-block padding cut-over.
      expect(
        sha256Hex(List.filled(55, 0x61)),
        '9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318',
      );
      expect(
        sha256Hex(List.filled(56, 0x61)),
        'b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a',
      );
      expect(
        sha256Hex(List.filled(64, 0x61)),
        'ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb',
      );
      expect(
        sha256Hex(List.filled(1000000, 0x61)),
        'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0',
      );
    });
  });

  group('lockTag', () {
    const rows = 'libvpx 1.0 aa https://x/a.tar.gz\nlz4 2.0 bb https://x/b\n';

    test('ignores comments, blank lines and column alignment', () {
      final base = lockTag(rows);
      expect(base, matches(RegExp(r'^deps-[0-9a-f]{16}$')));
      expect(lockTag('# note\n\n$rows'), base);
      expect(
        lockTag(
          'libvpx   1.0   aa   https://x/a.tar.gz\n'
          'lz4 2.0 bb https://x/b\n',
        ),
        base,
      );
    });

    test('changes when any row changes', () {
      expect(lockTag(rows.replaceFirst('1.0', '1.1')), isNot(lockTag(rows)));
      expect(lockTag(rows.replaceFirst('bb', 'bc')), isNot(lockTag(rows)));
    });

    test('rejects an empty lock', () {
      expect(
        () => lockTag('# only comments\n'),
        throwsA(isA<FetchException>()),
      );
    });
  });

  group('parseChecksums', () {
    final h = 'a' * 64;

    test('reads text and binary mode lines', () {
      expect(parseChecksums('$h  one.tar.gz\n${'B' * 64} *two.tar.gz\n'), {
        'one.tar.gz': h,
        'two.tar.gz': 'b' * 64,
      });
    });

    test('rejects duplicates and malformed lines', () {
      expect(
        () => parseChecksums('$h  x\n$h  x\n'),
        throwsA(isA<FetchException>()),
      );
      expect(
        () => parseChecksums('nothex  x\n'),
        throwsA(isA<FetchException>()),
      );
    });
  });

  group('fetch (end to end, file://)', () {
    late Directory tmp;
    late Directory release;
    late Directory out;
    late File lock;

    List<String> args([List<String> extra = const []]) => [
      '--base-url',
      release.uri.toString(),
      '--lock',
      lock.path,
      '--out',
      out.path,
      ...extra,
    ];

    Future<void> makeArchive(
      String platform,
      void Function(Directory) fill,
    ) async {
      final src = Directory('${tmp.path}/src-$platform');
      if (src.existsSync()) src.deleteSync(recursive: true);
      src.createSync();
      fill(src);
      await _sh('tar', [
        '-czf',
        '${release.path}/webm_texture_deps-$platform.tar.gz',
        '.',
      ], cwd: src.path);
    }

    void writeSums() {
      final lines = <String>[];
      for (final f in release.listSync().whereType<File>()) {
        final name = f.uri.pathSegments.last;
        if (name == 'SHA256SUMS') continue;
        lines.add('${sha256Hex(f.readAsBytesSync())}  $name');
      }
      File('${release.path}/SHA256SUMS')
          .writeAsStringSync('${lines.join('\n')}\n');
    }

    void fillAndroid(Directory d) {
      Directory('${d.path}/include').createSync();
      File('${d.path}/include/x.h').writeAsStringSync('//');
      Directory('${d.path}/arm64-v8a').createSync();
      File('${d.path}/arm64-v8a/libwebm_texture_deps.a')
          .writeAsStringSync('android-lib');
    }

    void fillIos(Directory d) {
      final x = Directory('${d.path}/WebmTextureDeps.xcframework')
        ..createSync();
      File('${x.path}/Info.plist').writeAsStringSync('plist');
    }

    setUp(() async {
      tmp = Directory.systemTemp.createTempSync('fetch_prebuilts_test');
      release = Directory('${tmp.path}/release')..createSync();
      out = Directory('${tmp.path}/prebuilt');
      lock = File('${tmp.path}/versions.lock')
        ..writeAsStringSync('libvpx 1.0 aa https://x/a.tar.gz\n');
      await makeArchive('android', fillAndroid);
      await makeArchive('ios', fillIos);
      writeSums();
    });

    tearDown(() => tmp.deleteSync(recursive: true));

    test('populates prebuilt/ and is idempotent', () async {
      expect(await _run(args()), 0);
      expect(
        File('${out.path}/android/arm64-v8a/libwebm_texture_deps.a')
            .readAsStringSync(),
        'android-lib',
      );
      expect(
        File('${out.path}/ios/WebmTextureDeps.xcframework/Info.plist')
            .existsSync(),
        isTrue,
      );
      final tag = lockTag(lock.readAsStringSync());
      expect(
        File('${out.path}/android/.release').readAsStringSync().trim(),
        tag,
      );

      // A second run must not touch the network: it still succeeds with the
      // release gone, while --force then fails because nothing can be fetched.
      release.deleteSync(recursive: true);
      expect(await _run(args()), 0);
      expect(await _run(args(['--force'])), 1);
    });

    test('a changed lock refetches', () async {
      expect(await _run(args()), 0);
      lock.writeAsStringSync('libvpx 1.1 aa https://x/a.tar.gz\n');
      release.deleteSync(recursive: true);
      expect(await _run(args()), 1);
    });

    test('single platform leaves the other alone', () async {
      expect(await _run(args(['--platform', 'ios'])), 0);
      expect(Directory('${out.path}/ios').existsSync(), isTrue);
      expect(Directory('${out.path}/android').existsSync(), isFalse);
    });

    test('a tampered archive fails and installs nothing', () async {
      final f = File('${release.path}/webm_texture_deps-android.tar.gz');
      final bytes = f.readAsBytesSync();
      bytes[bytes.length ~/ 2] ^= 0xff;
      f.writeAsBytesSync(bytes);
      expect(await _run(args(['--platform', 'android'])), 1);
      expect(Directory('${out.path}/android').existsSync(), isFalse);
    });

    test('a tampered checksum fails and keeps the existing install', () async {
      expect(await _run(args()), 0);
      final before = File('${out.path}/ios/.release').readAsStringSync();

      final sums = File('${release.path}/SHA256SUMS');
      sums.writeAsStringSync(
        sums
            .readAsLinesSync()
            .map((l) {
              return l.contains('ios')
                  ? '${'0' * 64}  ${l.split('  ').last}'
                  : l;
            })
            .join('\n'),
      );
      expect(await _run(args(['--force', '--platform', 'ios'])), 1);
      expect(File('${out.path}/ios/.release').readAsStringSync(), before);
      expect(
        File('${out.path}/ios/WebmTextureDeps.xcframework/Info.plist')
            .existsSync(),
        isTrue,
      );
    });

    test('a missing checksum entry fails', () async {
      final sums = File('${release.path}/SHA256SUMS');
      sums.writeAsStringSync(
        sums.readAsLinesSync().where((l) => !l.contains('ios')).join('\n'),
      );
      expect(await _run(args(['--platform', 'ios'])), 1);
    });

    test('an archive with the wrong layout fails', () async {
      await makeArchive(
        'ios',
        (d) => File('${d.path}/oops').writeAsStringSync('x'),
      );
      writeSums();
      expect(await _run(args(['--platform', 'ios'])), 1);
      expect(Directory('${out.path}/ios').existsSync(), isFalse);
    });

    test('path traversal in an archive is rejected', () async {
      final evil = File('${tmp.path}/evil.txt')..writeAsStringSync('x');
      final inner = Directory('${tmp.path}/inner')..createSync();
      await _sh('tar', [
        '-czPf',
        '${release.path}/webm_texture_deps-ios.tar.gz',
        '../evil.txt',
      ], cwd: inner.path);
      writeSums();
      expect(await _run(args(['--platform', 'ios'])), 1);
      expect(Directory('${out.path}/ios').existsSync(), isFalse);
      expect(evil.existsSync(), isTrue);
    });

    test('a symlink in an archive is rejected', () async {
      await makeArchive('ios', (d) {
        fillIos(d);
        Link('${d.path}/link').createSync('/etc/passwd');
      });
      writeSums();
      expect(await _run(args(['--platform', 'ios'])), 1);
      expect(Directory('${out.path}/ios').existsSync(), isFalse);
    });

    test('--print-tag prints the tag without touching the network', () async {
      expect(await _run(['--lock', lock.path, '--print-tag']), 0);
    });

    test('bad arguments exit 64', () async {
      expect(await _run(['--platform', 'windows']), 64);
      expect(await _run(['--nope']), 64);
    });
  });
}
