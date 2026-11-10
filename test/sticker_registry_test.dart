import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_bucket.dart';
import 'package:webm_texture/src/sticker_registry.dart';
import 'package:webm_texture/src/sticker_source.dart';

import 'fake_host.dart';

const _grace = Duration(seconds: 2);
const _b128 = StickerBucket(128, 128);
const _b256 = StickerBucket(256, 256);
const _file = StickerSource.file('/a.webm');

void main() {
  late FakeHost host;
  late StickerRegistry registry;

  setUp(() {
    host = FakeHost();
    registry = StickerRegistry(host: host, gracePeriod: _grace);
  });

  Future<StickerLease> acquire(
    String key, [
    StickerBucket bucket = _b128,
    StickerSource source = _file,
  ]) => registry.acquire(key: key, source: source, bucket: bucket);

  group('dedupe', () {
    test('same key and bucket share one animation and texture', () async {
      final a = await acquire('k');
      final b = await acquire('k');
      expect(host.created, hasLength(1));
      expect(b.animId, a.animId);
      expect(b.textureId, a.textureId);
      expect(registry.entryCount, 1);
    });

    test('different bucket or key creates a separate animation', () async {
      final a = await acquire('k');
      final b = await acquire('k', _b256);
      final c = await acquire('other');
      expect(host.created, hasLength(3));
      expect({a.animId, b.animId, c.animId}, hasLength(3));
      expect(b.bucket, _b256);
    });

    test('concurrent acquires share one platform call', () async {
      host.gate = Completer<void>();
      final fa = acquire('k');
      final fb = acquire('k');
      host.gate!.complete();
      final leases = await Future.wait([fa, fb]);
      expect(host.created, hasLength(1));
      expect(leases[0].textureId, leases[1].textureId);
    });

    test('passes the bucket, key and source to the platform', () async {
      final data = Uint8List.fromList([1, 2, 3]);
      await acquire('p', _b256, StickerSource.bytes(data));
      await acquire('q');
      final (key, source, w, h) = host.created[0];
      expect((key, w, h), ('p', 256, 256));
      expect(source.bytes, data);
      expect(source.path, isNull);
      expect(host.created[1].$2.path, '/a.webm');
      expect(host.created[1].$2.bytes, isNull);
    });

    test('a failed creation reaches every waiter and is not kept', () async {
      host.gate = Completer<void>();
      host.failWith = PlatformException(code: 'WT_ERR_LIMIT');
      final fa = acquire('k');
      final fb = acquire('k');
      final ra = expectLater(fa, throwsA(isA<PlatformException>()));
      final rb = expectLater(fb, throwsA(isA<PlatformException>()));
      host.gate!.complete();
      await Future.wait([ra, rb]);
      expect(host.created, hasLength(1));
      expect(registry.entryCount, 0);

      host.failWith = null;
      final lease = await acquire('k');
      expect(host.created, hasLength(2));
      expect(lease.animId, isNonZero);
    });
  });

  group('refcount and grace period', () {
    testWidgets('disposes only after the last release plus the grace', (
      tester,
    ) async {
      late StickerLease a, b;
      acquire('k').then((l) => a = l);
      acquire('k').then((l) => b = l);
      await tester.pump();

      a.release();
      await tester.pump(_grace * 2);
      expect(host.disposed, isEmpty, reason: 'b still holds it');

      b.release();
      await tester.pump(_grace - const Duration(milliseconds: 1));
      expect(host.disposed, isEmpty);
      await tester.pump(const Duration(milliseconds: 1));
      expect(host.disposed, [a.animId]);
      expect(registry.entryCount, 0);
    });

    testWidgets('release is idempotent', (tester) async {
      late StickerLease a, b;
      acquire('k').then((l) => a = l);
      acquire('k').then((l) => b = l);
      await tester.pump();

      a.release();
      a.release();
      expect(a.isReleased, isTrue);
      await tester.pump(_grace * 2);
      expect(host.disposed, isEmpty, reason: 'double release is one release');

      b.release();
      await tester.pump(_grace);
      expect(host.disposed, hasLength(1));
    });

    testWidgets('reacquiring within the grace reuses the animation', (
      tester,
    ) async {
      late StickerLease a, b;
      acquire('k').then((l) => a = l);
      await tester.pump();
      a.release();
      await tester.pump(_grace - const Duration(milliseconds: 1));

      acquire('k').then((l) => b = l);
      await tester.pump();
      expect(b.animId, a.animId);
      expect(host.created, hasLength(1));

      await tester.pump(_grace * 5);
      expect(host.disposed, isEmpty, reason: 'grace timer was cancelled');

      b.release();
      await tester.pump(_grace);
      expect(host.disposed, [a.animId], reason: 'new grace after re-release');
    });

    testWidgets('acquiring after the dispose creates a fresh animation', (
      tester,
    ) async {
      late StickerLease a, b;
      acquire('k').then((l) => a = l);
      await tester.pump();
      a.release();
      await tester.pump(_grace);

      acquire('k').then((l) => b = l);
      await tester.pump();
      expect(host.created, hasLength(2));
      expect(b.animId, isNot(a.animId));
    });

    testWidgets('release during creation disposes after the grace', (
      tester,
    ) async {
      host.gate = Completer<void>();
      StickerLease? a;
      acquire('k').then((l) => a = l);
      await tester.pump();
      expect(a, isNull);
      host.gate!.complete();
      await tester.pump();
      a!.release();
      await tester.pump(_grace);
      expect(host.disposed, [a!.animId]);
    });

    testWidgets('animations expire independently', (tester) async {
      late StickerLease a, b;
      acquire('a').then((l) => a = l);
      acquire('b').then((l) => b = l);
      await tester.pump();
      a.release();
      await tester.pump(_grace ~/ 2);
      b.release();
      await tester.pump(_grace ~/ 2);
      expect(host.disposed, [a.animId]);
      await tester.pump(_grace ~/ 2);
      expect(host.disposed, [a.animId, b.animId]);
    });

    testWidgets('platform dispose errors are swallowed', (tester) async {
      final failing = _ThrowingDisposeHost();
      final r = StickerRegistry(host: failing, gracePeriod: _grace);
      late StickerLease a;
      r.acquire(key: 'k', source: _file, bucket: _b128).then((l) => a = l);
      await tester.pump();
      a.release();
      await tester.pump(_grace);
      await tester.pump();
      expect(failing.disposeCalls, 1);
    });
  });

  group('active', () {
    test('plays while any lease is active', () async {
      final a = await acquire('k');
      final b = await acquire('k');
      final id = a.animId;
      a.setActive(true);
      b.setActive(true);
      a.setActive(true); // no change
      expect(host.activeCalls, [(id, true)]);
      expect(registry.activeLeases.value, 0, reason: 'published later');
      await pumpEventQueue();
      expect(registry.activeLeases.value, 2);
      expect(registry.activeAnimations.value, 1);
      a.setActive(false);
      expect(host.activeCalls, [(id, true)]);
      b.setActive(false);
      expect(host.activeCalls, [(id, true), (id, false)]);
      await pumpEventQueue();
      expect(registry.activeLeases.value, 0);
      expect(registry.activeAnimations.value, 0);
    });

    test('releasing an active lease deactivates it', () async {
      final a = await acquire('k');
      a.setActive(true);
      a.release();
      expect(host.activeCalls, [(a.animId, true), (a.animId, false)]);
      a.setActive(true); // released: ignored
      expect(host.activeCalls, hasLength(2));
      await pumpEventQueue();
      expect(registry.activeLeases.value, 0);
    });

    test('setActive errors are swallowed', () async {
      final throwing = _ThrowingActiveHost();
      final r = StickerRegistry(host: throwing, gracePeriod: _grace);
      final a = await r.acquire(key: 'k', source: _file, bucket: _b128);
      a.setActive(true);
      await pumpEventQueue();
      expect(r.activeAnimations.value, 1);
      await r.dispose();
    });
  });

  group('owner', () {
    test('owners get separate animations', () async {
      final o1 = Object();
      final shared = await acquire('k');
      final a = await registry.acquire(
        key: 'k',
        source: _file,
        bucket: _b128,
        owner: o1,
      );
      final b = await registry.acquire(
        key: 'k',
        source: _file,
        bucket: _b128,
        owner: o1,
      );
      final c = await registry.acquire(
        key: 'k',
        source: _file,
        bucket: _b128,
        owner: Object(),
      );
      expect(host.created, hasLength(3));
      expect(a.animId, b.animId);
      expect({shared.animId, a.animId, c.animId}, hasLength(3));
      // Every animation keeps the content key for the platform cache.
      expect(host.created.map((c) => c.$1), everyElement('k'));
    });
  });

  group('dispose', () {
    test('disposes everything immediately and rejects new claims', () async {
      final a = await acquire('a');
      final b = await acquire('b');
      a.release();
      await registry.dispose();
      expect(host.disposed, unorderedEquals([a.animId, b.animId]));
      expect(registry.entryCount, 0);
      await expectLater(acquire('c'), throwsStateError);
      b.release(); // stale lease, must not throw or double dispose
      expect(host.disposed, hasLength(2));
    });
  });
}

class _ThrowingDisposeHost extends FakeHost {
  int disposeCalls = 0;

  @override
  Future<void> dispose(int animId) async {
    disposeCalls++;
    throw PlatformException(code: 'BOOM');
  }
}

class _ThrowingActiveHost extends FakeHost {
  @override
  Future<void> setActive(int animId, bool active) async =>
      throw PlatformException(code: 'BOOM');
}
