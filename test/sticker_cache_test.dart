// StickerCache against a fake host and a fake native layer, and (when the
// host build of the core is available) against the real one:
//
//   tool/build_host_ffi_lib.sh
//   WEBM_TEXTURE_LIB=build/ffi/src/libwebm_texture.dylib fvm flutter test test/sticker_cache_test.dart
@TestOn('vm')
library;

import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_cache.dart';
import 'package:webm_texture/src/sticker_config.dart';
import 'package:webm_texture/src/sticker_runtime.dart';
import 'package:webm_texture/src/sticker_tier.dart';
import 'package:webm_texture/webm_texture.dart'
    show StickerException, WebmStickers;

import 'fake_host.dart';

class FakeNative implements CacheNative {
  final List<(String, int)> applied = [];
  final List<String> cleared = [];
  final List<String> statted = [];
  CacheStats result = const CacheStats(fileCount: 3, totalBytes: 1234);

  @override
  Future<void> applyBudget(String directory, int budgetBytes) async =>
      applied.add((directory, budgetBytes));

  @override
  Future<CacheStats> stats(String directory) async {
    statted.add(directory);
    return result;
  }

  @override
  Future<void> clear(String directory) async => cleared.add(directory);
}

void main() {
  group('with fakes', () {
    late FakeHost host;
    late FakeNative native;
    late StickerCache cache;

    setUp(() {
      host = FakeHost();
      native = FakeNative();
      cache = StickerCache(host: host, native: native);
    });

    test('configure sets the directory on the host, then the budget', () async {
      await cache.configure(
        const StickerConfig(
          cacheDirectory: '/data/stickers',
          diskBudgetBytes: 50,
        ),
      );
      expect(host.cacheConfigs, ['/data/stickers']);
      expect(native.applied, [('/data/stickers', 50)]);
    });

    test('configure without a directory keeps the platform default', () async {
      await cache.configure(const StickerConfig());
      expect(host.cacheConfigs, [null]);
      expect(native.applied, [
        ('/fake/cache', StickerConfig.defaultDiskBudgetBytes),
      ]);
      expect(StickerConfig.defaultDiskBudgetBytes, 200 * 1024 * 1024);
    });

    test('stats and clear work on the directory in use', () async {
      await cache.configure(const StickerConfig(cacheDirectory: '/elsewhere'));
      expect(
        await cache.stats(),
        const CacheStats(fileCount: 3, totalBytes: 1234),
      );
      await cache.clear();
      expect(native.statted, ['/elsewhere']);
      expect(native.cleared, ['/elsewhere']);
      // Asking doesn't change the directory.
      expect(host.cacheConfigs.last, isNull);
    });

    test('a host failure surfaces and applies no budget', () async {
      final failing = StickerCache(host: _FailingHost(), native: native);
      await expectLater(
        failing.configure(const StickerConfig(cacheDirectory: 'relative')),
        throwsA(isA<StateError>()),
      );
      expect(native.applied, isEmpty);
    });

    test('WebmStickers delegates to the runtime\'s cache', () async {
      StickerRuntime.debugOverride = StickerRuntime(
        cache: cache,
        tiers: StickerTierController(
          host: FakeHost(),
          signalChanges: () => const Stream.empty(),
        ),
      );
      addTearDown(() => StickerRuntime.debugOverride = null);
      await WebmStickers.configure(
        const StickerConfig(cacheDirectory: '/x', diskBudgetBytes: 7),
      );
      expect(native.applied, [('/x', 7)]);
      expect(await WebmStickers.cacheStats(), native.result);
      await WebmStickers.clearCache();
      expect(native.cleared, ['/x']);
    });

    test('CacheStats compares by value', () {
      expect(
        const CacheStats(fileCount: 1, totalBytes: 2),
        const CacheStats(fileCount: 1, totalBytes: 2),
      );
      expect(
        const CacheStats(fileCount: 1, totalBytes: 2),
        isNot(const CacheStats(fileCount: 1, totalBytes: 3)),
      );
    });
  });

  group('over FFI', () {
    final lib = Platform.environment['WEBM_TEXTURE_LIB'];
    final skip = lib == null || !File(lib).existsSync()
        ? 'set WEBM_TEXTURE_LIB (tool/build_host_ffi_lib.sh)'
        : null;

    late Directory dir;
    late StickerCache cache;

    setUp(() {
      dir = Directory.systemTemp.createTempSync('wt_cache_dart_');
      cache = StickerCache(host: FakeHost());
    });
    tearDown(() => dir.deleteSync(recursive: true));

    // A file of [size] bytes last used [ageSeconds] ago.
    void make(String name, int size, int ageSeconds) {
      final f = File('${dir.path}/$name')
        ..writeAsBytesSync(List.filled(size, 0x78));
      f.setLastModifiedSync(
        DateTime.now().subtract(Duration(seconds: ageSeconds)),
      );
    }

    List<String> names() =>
        dir.listSync().map((e) => e.uri.pathSegments.last).toList()..sort();

    test('stats counts the cache files', skip: skip, () async {
      await cache.configure(StickerConfig(cacheDirectory: dir.path));
      make('a.wsc', 100, 0);
      make('b.wsc', 50, 0);
      make('notes.txt', 999, 0);
      expect(
        await cache.stats(),
        const CacheStats(fileCount: 2, totalBytes: 150),
      );
    });

    test(
      'configure evicts the least recently used down to the budget',
      skip: skip,
      () async {
        make('old.wsc', 100, 300);
        make('mid.wsc', 100, 200);
        make('new.wsc', 100, 100);
        await cache.configure(
          StickerConfig(cacheDirectory: dir.path, diskBudgetBytes: 250),
        );
        expect(names(), ['mid.wsc', 'new.wsc']);
        await cache.configure(
          StickerConfig(cacheDirectory: dir.path, diskBudgetBytes: 100),
        );
        expect(names(), ['new.wsc']);
        // Restore the default so other tests in this process aren't affected.
        await cache.configure(StickerConfig(cacheDirectory: dir.path));
      },
    );

    test('clear deletes cache files only', skip: skip, () async {
      await cache.configure(StickerConfig(cacheDirectory: dir.path));
      make('a.wsc', 10, 0);
      make('a.wsc.1.0.tmp', 10, 0);
      make('keep.txt', 10, 0);
      await cache.clear();
      expect(names(), ['keep.txt']);
      expect(
        await cache.stats(),
        const CacheStats(fileCount: 0, totalBytes: 0),
      );
    });

    test('a missing directory is an empty cache', skip: skip, () async {
      final gone = '${dir.path}/never-created';
      await cache.configure(StickerConfig(cacheDirectory: gone));
      expect(
        await cache.stats(),
        const CacheStats(fileCount: 0, totalBytes: 0),
      );
      await cache.clear();
    });

    test('a path that is a file is a WT_ERR_IO', skip: skip, () async {
      make('file', 1, 0);
      await cache.configure(StickerConfig(cacheDirectory: '${dir.path}/file'));
      await expectLater(
        cache.stats(),
        throwsA(
          isA<StickerException>().having((e) => e.name, 'name', 'WT_ERR_IO'),
        ),
      );
    });
  });
}

class _FailingHost extends FakeHost {
  @override
  Future<String> configureCache(String? directory) =>
      Future.error(StateError('rejected'));
}
