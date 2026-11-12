import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sha1.dart';
import 'package:webm_texture/src/sticker_resolver.dart';
import 'package:webm_texture/webm_texture.dart';

import 'fake_fetcher.dart';

class FakeBundle extends CachingAssetBundle {
  final Map<String, Uint8List> assets = {};
  final List<String> loads = [];

  @override
  Future<ByteData> load(String key) async {
    loads.add(key);
    final data = assets[key];
    if (data == null) throw FlutterError('Unable to load asset: $key');
    return ByteData.sublistView(data);
  }
}

Uint8List _bytes(String s) => Uint8List.fromList(utf8.encode(s));

void main() {
  group('sha1Hex', () {
    test('matches the standard vectors', () {
      expect(sha1Hex(Uint8List(0)), 'da39a3ee5e6b4b0d3255bfef95601890afd80709');
      expect(
        sha1Hex(_bytes('abc')),
        'a9993e364706816aba3e25717850c26c9cd0d89d',
      );
      expect(
        sha1Hex(
          _bytes('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'),
        ),
        '84983e441c3bd26ebaae4aa1f95129e5e54670f1',
      );
      expect(
        sha1Hex(_bytes('a' * 1000000)),
        '34aa973cd4c4daa4f61eeb2bdbad27316534016f',
      );
    });

    test('handles lengths around the padding boundary', () {
      // sha1 of 'a' * n for n = 55, 56, 63, 64, 65, from `shasum`.
      const expected = {
        55: 'c1c8bbdc22796e28c0e15163d20899b65621d65a',
        56: 'c2db330f6083854c99d4b5bfb6e8f29f201be699',
        63: '03f09f5b158a7a8cdad920bddc29b81c18a551f5',
        64: '0098ba824b5c16427bd7a1122a5a442a25ec644d',
        65: '11655326c708d70319be2610e8a57d9a5b959d3b',
      };
      for (final MapEntry(key: n, value: hash) in expected.entries) {
        expect(sha1Hex(_bytes('a' * n)), hash, reason: 'length $n');
      }
    });

    test('hashes a view into a larger buffer', () {
      final whole = _bytes('xxabcxx');
      expect(
        sha1Hex(Uint8List.sublistView(whole, 2, 5)),
        'a9993e364706816aba3e25717850c26c9cd0d89d',
      );
    });
  });

  group('equality', () {
    test('file, asset and network compare by value', () {
      expect(const StickerSource.file('/a'), const StickerSource.file('/a'));
      expect(
        const StickerSource.file('/a'),
        isNot(const StickerSource.file('/b')),
      );
      expect(
        const StickerSource.asset('a.webm', package: 'p'),
        const StickerSource.asset('a.webm', package: 'p'),
      );
      expect(
        const StickerSource.asset('a.webm'),
        isNot(const StickerSource.asset('a.webm', package: 'p')),
      );
      expect(
        const StickerSource.network('u', headers: {'a': 'b'}),
        const StickerSource.network('u', headers: {'a': 'b'}),
      );
      expect(
        const StickerSource.network('u', headers: {'a': 'b'}),
        isNot(const StickerSource.network('u', headers: {'a': 'c'})),
      );
    });

    test('bytes compare by key, or by instance without one', () {
      final a = Uint8List(4);
      final copy = Uint8List(4);
      expect(StickerSource.bytes(a), StickerSource.bytes(a));
      expect(StickerSource.bytes(a), isNot(StickerSource.bytes(copy)));
      expect(
        StickerSource.bytes(a, key: 'k'),
        StickerSource.bytes(copy, key: 'k'),
      );
    });

    test('an asset key gains the packages/ prefix for a package', () {
      expect(const AssetStickerSource('a.webm').assetKey, 'a.webm');
      expect(
        const AssetStickerSource('a.webm', package: 'p').assetKey,
        'packages/p/a.webm',
      );
    });
  });

  group('StickerResolver', () {
    late FakeFetcher fetcher;
    late FakeBundle bundle;
    late StickerResolver resolver;

    setUp(() {
      fetcher = FakeFetcher();
      bundle = FakeBundle();
      resolver = StickerResolver(fetcher: fetcher);
    });

    test('a file is keyed by its path and passed through', () async {
      const source = StickerSource.file('/a.webm');
      final r = await resolver.resolve(source);
      expect(r.key, 'file:/a.webm');
      expect(r.source, source);
    });

    test('bytes are keyed by their SHA-1, equal copies share a key', () async {
      final a = await resolver.resolve(StickerSource.bytes(_bytes('abc')));
      final b = await resolver.resolve(StickerSource.bytes(_bytes('abc')));
      final c = await resolver.resolve(StickerSource.bytes(_bytes('abd')));
      expect(a.key, 'sha1:a9993e364706816aba3e25717850c26c9cd0d89d');
      expect(b.key, a.key);
      expect(c.key, isNot(a.key));
    });

    test('bytes with a caller key use it without hashing', () async {
      final r = await resolver.resolve(
        StickerSource.bytes(_bytes('abc'), key: 'mine'),
      );
      expect(r.key, 'key:mine');
    });

    test('an asset is loaded once and keyed by content', () async {
      bundle.assets['packages/p/a.webm'] = _bytes('abc');
      final source = StickerSource.asset(
        'a.webm',
        package: 'p',
        bundle: bundle,
      );
      final r = await resolver.resolve(source);
      final again = await resolver.resolve(source);
      expect(bundle.loads, ['packages/p/a.webm']);
      expect(r.key, 'sha1:a9993e364706816aba3e25717850c26c9cd0d89d');
      expect(again, same(r));
      expect((r.source as BytesStickerSource).data, _bytes('abc'));
    });

    test('a missing asset fails and is retried next time', () async {
      final source = StickerSource.asset('nope.webm', bundle: bundle);
      await expectLater(resolver.resolve(source), throwsFlutterError);
      bundle.assets['nope.webm'] = _bytes('abc');
      final r = await resolver.resolve(source);
      expect(r.key, startsWith('sha1:'));
      expect(bundle.loads, hasLength(2));
    });

    test('network bytes: fetched with headers, keyed by SHA-1', () async {
      fetcher.result = StickerFetchResult.bytes(_bytes('abc'));
      final r = await resolver.resolve(
        const StickerSource.network(
          'https://x.test/a.webm',
          headers: {'Authorization': 't'},
        ),
      );
      expect(fetcher.calls.single.$1, Uri.parse('https://x.test/a.webm'));
      expect(fetcher.calls.single.$2, {'Authorization': 't'});
      expect(r.key, 'sha1:a9993e364706816aba3e25717850c26c9cd0d89d');
      expect(r.source, isA<BytesStickerSource>());
    });

    test('network file: keyed by path, handed on as a file source', () async {
      fetcher.result = const StickerFetchResult.file('/cache/a.webm');
      final r = await resolver.resolve(const StickerSource.network('u'));
      expect(r.key, 'file:/cache/a.webm');
      expect(r.source, const StickerSource.file('/cache/a.webm'));
    });

    test('a caller key names network content either way', () async {
      fetcher.result = StickerFetchResult.bytes(_bytes('abc'));
      final a = await resolver.resolve(
        const StickerSource.network('u1', key: 'k'),
      );
      fetcher.result = const StickerFetchResult.file('/f');
      final b = await resolver.resolve(
        const StickerSource.network('u2', key: 'k2'),
      );
      expect((a.key, b.key), ('key:k', 'key:k2'));
    });

    test('concurrent and repeated resolves share one fetch', () async {
      fetcher.gate = Completer<void>();
      const source = StickerSource.network('u');
      final a = resolver.resolve(source);
      final b = resolver.resolve(source);
      fetcher.gate!.complete();
      expect(await a, same(await b));
      await resolver.resolve(source);
      expect(fetcher.calls, hasLength(1));
      await resolver.resolve(const StickerSource.network('other'));
      expect(fetcher.calls, hasLength(2));
    });

    test('a failed fetch surfaces the error and is not cached', () async {
      fetcher.failWith = const SocketLikeError('offline');
      const source = StickerSource.network('u');
      await expectLater(
        resolver.resolve(source),
        throwsA(isA<SocketLikeError>()),
      );
      fetcher.failWith = null;
      await resolver.resolve(source);
      expect(fetcher.calls, hasLength(2));
    });

    // T6.1 (S-07): bytes the core would refuse (over its 2 MiB max_bytes) are
    // refused before the UI isolate hashes them or the cache pins them.
    test(
      'fetched bytes over the input limit are refused, not cached',
      () async {
        fetcher.result = StickerFetchResult.bytes(
          Uint8List(maxFetchedStickerBytes + 1),
        );
        const source = StickerSource.network('u');
        await expectLater(
          resolver.resolve(source),
          throwsA(
            isA<StickerException>().having(
              (e) => e.name,
              'name',
              'WT_ERR_LIMIT',
            ),
          ),
        );
        fetcher.result = StickerFetchResult.bytes(
          Uint8List(maxFetchedStickerBytes),
        );
        expect((await resolver.resolve(source)).key, startsWith('sha1:'));
        expect(fetcher.calls, hasLength(2));
      },
    );

    // T6.1 (S-06): iOS hands keys and paths to the core as C strings, so a
    // NUL would cut them short and two keys could share one cache file.
    test('a key or path with a NUL is refused', () async {
      for (final source in [
        StickerSource.bytes(_bytes('abc'), key: 'k\u0000x'),
        const StickerSource.network('u', key: 'k\u0000x'),
        const StickerSource.file('/a\u0000.webm'),
      ]) {
        await expectLater(
          resolver.resolve(source),
          throwsA(isA<ArgumentError>()),
        );
      }
      fetcher.result = const StickerFetchResult.file('/cache/a\u0000.webm');
      await expectLater(
        resolver.resolve(const StickerSource.network('u')),
        throwsA(isA<ArgumentError>()),
      );
      expect(fetcher.calls, hasLength(1));
    });

    test('network without a fetcher is a StateError naming the url', () async {
      resolver.fetcher = null;
      await expectLater(
        resolver.resolve(const StickerSource.network('https://x.test/a')),
        throwsA(
          isA<StateError>().having(
            (e) => e.message,
            'message',
            contains('https://x.test/a'),
          ),
        ),
      );
    });

    test('the cache keeps the most recently used results', () async {
      resolver = StickerResolver(fetcher: fetcher, cacheSize: 2);
      await resolver.resolve(const StickerSource.network('a'));
      await resolver.resolve(const StickerSource.network('b'));
      await resolver.resolve(const StickerSource.network('a')); // a is fresh
      await resolver.resolve(const StickerSource.network('c')); // evicts b
      expect(fetcher.calls, hasLength(3));
      await resolver.resolve(const StickerSource.network('a'));
      expect(fetcher.calls, hasLength(3));
      await resolver.resolve(const StickerSource.network('b'));
      expect(fetcher.calls, hasLength(4));
    });
  });
}

class SocketLikeError implements Exception {
  const SocketLikeError(this.message);
  final String message;
}
