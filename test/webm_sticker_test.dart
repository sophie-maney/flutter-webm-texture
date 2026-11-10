import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_registry.dart';
import 'package:webm_texture/src/sticker_runtime.dart';
import 'package:webm_texture/webm_texture.dart';

import 'fake_fetcher.dart';
import 'fake_host.dart';

const _grace = Duration(seconds: 2);

StickerInfo _info(int w, int h) => StickerInfo(
  format: StickerFormat.webm,
  codec: StickerCodec.vp9,
  width: w,
  height: h,
  frameCount: 30,
  duration: const Duration(seconds: 1),
  fps: 30,
  hasAlpha: true,
  hasAudio: false,
  truncated: false,
);

void main() {
  late FakeHost host;
  late StickerRuntime runtime;
  late Map<String, Object> probes; // path -> StickerInfo or error
  late int probeCalls;
  Completer<void>? probeGate;

  setUp(() {
    host = FakeHost();
    probes = {};
    probeCalls = 0;
    probeGate = null;
    runtime = StickerRuntime(
      registry: StickerRegistry(host: host, gracePeriod: _grace),
      prober: (source) async {
        probeCalls++;
        await probeGate?.future;
        final path = switch (source) {
          FileStickerSource(:final path) => path,
          BytesStickerSource(:final key) => 'bytes:$key',
          _ => throw ArgumentError('unresolved $source'),
        };
        final r = probes[path] ?? _info(512, 512);
        if (r is StickerInfo) return r;
        throw r;
      },
    );
    StickerRuntime.debugOverride = runtime;
  });

  tearDown(() async {
    StickerRuntime.debugOverride = null;
  });

  /// Lets the probe and the platform call finish and the widget rebuild.
  Future<void> settle(WidgetTester tester) async {
    for (var i = 0; i < 3; i++) {
      await tester.pump();
    }
  }

  /// Unmounts everything and lets the grace timers run, so no timer is left.
  Future<void> finish(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await tester.pump(_grace);
    await runtime.dispose();
  }

  /// Animation ids the platform was last told are playing.
  Set<int> playing() {
    final state = <int, bool>{};
    for (final (id, active) in host.activeCalls) {
      state[id] = active;
    }
    return {
      for (final e in state.entries)
        if (e.value) e.key,
    };
  }

  Widget app(Widget child) => Directionality(
    textDirection: TextDirection.ltr,
    child: MediaQuery(
      data: const MediaQueryData(size: Size(800, 600), devicePixelRatio: 2),
      child: child,
    ),
  );

  Widget sticker(
    String path, {
    Key? key,
    double size = 100,
    bool autoplay = true,
    WebmStickerController? controller,
    Widget? placeholder,
    StickerErrorWidgetBuilder? errorBuilder,
  }) => WebmSticker(
    key: key,
    source: StickerSource.file(path),
    width: size,
    height: size,
    autoplay: autoplay,
    controller: controller,
    placeholder: placeholder,
    errorBuilder: errorBuilder,
  );

  group('loading', () {
    testWidgets('placeholder until the texture is ready, then the texture', (
      tester,
    ) async {
      host.gate = Completer<void>();
      await tester.pumpWidget(
        app(Center(child: sticker('/a', placeholder: const Text('wait')))),
      );
      await settle(tester);
      expect(find.text('wait'), findsOneWidget);
      expect(find.byType(Texture), findsNothing);
      // 100 logical px at DPR 2 = 200 physical, rounded up to 256.
      expect(host.created.single.$1, 'file:/a');
      expect((host.created.single.$3, host.created.single.$4), (256, 256));

      host.gate!.complete();
      await settle(tester);
      expect(find.text('wait'), findsNothing);
      final texture = tester.widget<Texture>(find.byType(Texture));
      expect(texture.textureId, 101);
      expect(tester.getSize(find.byType(WebmSticker)), const Size(100, 100));
      await finish(tester);
    });

    testWidgets('the bucket keeps the sticker aspect ratio', (tester) async {
      probes['/wide'] = _info(300, 200);
      await tester.pumpWidget(app(Center(child: sticker('/wide'))));
      await settle(tester);
      // Contained in 200x200 physical: 200x133 -> longer side 256.
      expect((host.created.single.$3, host.created.single.$4), (256, 171));
      await finish(tester);
    });

    testWidgets('a missing dimension follows from the aspect ratio', (
      tester,
    ) async {
      probes['/wide'] = _info(300, 200);
      await tester.pumpWidget(
        app(
          const Center(
            child: WebmSticker(source: StickerSource.file('/wide'), width: 90),
          ),
        ),
      );
      await settle(tester);
      expect(tester.getSize(find.byType(WebmSticker)), const Size(90, 60));
      await finish(tester);
    });

    testWidgets('a resize re-acquires at the new bucket', (tester) async {
      await tester.pumpWidget(app(Center(child: sticker('/a'))));
      await settle(tester);
      await tester.pumpWidget(app(Center(child: sticker('/a', size: 200))));
      await settle(tester);
      expect(host.created.map((c) => c.$3), [256, 512]);
      expect(tester.widget<Texture>(find.byType(Texture)).textureId, 102);
      await tester.pump(_grace);
      expect(host.disposed, [1]);
      await finish(tester);
    });

    testWidgets('identical stickers share one animation', (tester) async {
      await tester.pumpWidget(
        app(Row(children: [sticker('/a'), sticker('/a'), sticker('/b')])),
      );
      await settle(tester);
      expect(host.created.map((c) => c.$1), ['file:/a', 'file:/b']);
      expect(probeCalls, 2);
      await finish(tester);
    });
  });

  group('errors', () {
    testWidgets('a probe failure shows errorBuilder', (tester) async {
      probes['/bad'] = const StickerException(-6, 'WT_ERR_LIMIT');
      await tester.pumpWidget(
        app(
          Center(
            child: sticker(
              '/bad',
              placeholder: const Text('wait'),
              errorBuilder: (_, e) =>
                  Text('error ${(e as StickerException).name}'),
            ),
          ),
        ),
      );
      await settle(tester);
      expect(find.text('error WT_ERR_LIMIT'), findsOneWidget);
      expect(host.created, isEmpty);
      await finish(tester);
    });

    testWidgets('a platform failure becomes a StickerException', (
      tester,
    ) async {
      host.failWith = PlatformException(code: 'WT_ERR_DECODE', message: 'x');
      Object? error;
      await tester.pumpWidget(
        app(
          Center(
            child: sticker(
              '/a',
              errorBuilder: (_, e) {
                error = e;
                return const Text('error');
              },
            ),
          ),
        ),
      );
      await settle(tester);
      expect(find.text('error'), findsOneWidget);
      expect(
        error,
        isA<StickerException>()
            .having((e) => e.code, 'code', -9)
            .having((e) => e.name, 'name', 'WT_ERR_DECODE'),
      );
      await finish(tester);
    });

    testWidgets('a new source clears the error', (tester) async {
      probes['/bad'] = const StickerException(-3, 'WT_ERR_INVALID_DATA');
      Widget build(String path) => app(
        Center(child: sticker(path, errorBuilder: (_, _) => const Text('e'))),
      );
      await tester.pumpWidget(build('/bad'));
      await settle(tester);
      expect(find.text('e'), findsOneWidget);
      await tester.pumpWidget(build('/a'));
      await settle(tester);
      expect(find.text('e'), findsNothing);
      expect(find.byType(Texture), findsOneWidget);
      await finish(tester);
    });
  });

  group('visibility', () {
    testWidgets('an on-screen sticker plays', (tester) async {
      await tester.pumpWidget(app(Center(child: sticker('/a'))));
      await settle(tester);
      expect(playing(), {1});
      expect(WebmStickers.debugActiveCount.value, 1);
      await finish(tester);
      expect(playing(), isEmpty);
      expect(WebmStickers.debugActiveCount.value, 0);
    });

    testWidgets('the counter can drive a widget while a list scrolls', (
      tester,
    ) async {
      // Scrolling disposes items during layout, inside the build scope; the
      // counter must not notify its listeners right then.
      final controller = ScrollController();
      await tester.pumpWidget(
        app(
          Column(
            children: [
              ValueListenableBuilder<int>(
                valueListenable: WebmStickers.debugActiveCount,
                builder: (_, n, _) => Text('active $n'),
              ),
              Expanded(
                child: ListView.builder(
                  controller: controller,
                  itemCount: 100,
                  itemExtent: 100,
                  itemBuilder: (_, i) => Align(
                    alignment: Alignment.centerLeft,
                    child: sticker('/$i'),
                  ),
                ),
              ),
            ],
          ),
        ),
      );
      await settle(tester);
      final before = find.textContaining('active ');
      expect(tester.widget<Text>(before).data, isNot('active 0'));
      controller.jumpTo(5000);
      await settle(tester);
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(app(const SizedBox()));
      await settle(tester);
      expect(tester.takeException(), isNull);
      expect(WebmStickers.debugActiveCount.value, 0);
      await finish(tester);
    });

    testWidgets('only the on-screen items of a list play', (tester) async {
      final controller = ScrollController();
      // 600 px tall viewport, 100 px items: items 0-5 on screen, the cache
      // extent (250 px) builds 6-8 too.
      await tester.pumpWidget(
        app(
          ListView.builder(
            controller: controller,
            itemCount: 200,
            itemExtent: 100,
            itemBuilder: (_, i) =>
                Align(alignment: Alignment.centerLeft, child: sticker('/$i')),
          ),
        ),
      );
      await settle(tester);
      expect(host.created.length, greaterThan(6));
      Set<String> playingKeys() => {
        for (final id in playing()) host.created[id - 1].$1,
      };
      expect(playingKeys(), {for (var i = 0; i < 6; i++) 'file:/$i'});
      expect(WebmStickers.debugActiveCount.value, 6);

      // Half an item: 0 is half visible, 6 starts to show.
      controller.jumpTo(550);
      await settle(tester);
      await settle(tester);
      expect(playingKeys(), {for (var i = 5; i < 12; i++) 'file:/$i'});
      expect(WebmStickers.debugActiveCount.value, 7);

      controller.jumpTo(5000);
      await settle(tester);
      await settle(tester);
      expect(playingKeys(), {for (var i = 50; i < 56; i++) 'file:/$i'});
      expect(WebmStickers.debugActiveCount.value, 6);
      await finish(tester);
    });

    testWidgets('a clipped-away sticker does not play', (tester) async {
      await tester.pumpWidget(
        app(
          Center(
            child: ClipRect(
              child: SizedBox(
                width: 50,
                height: 50,
                child: OverflowBox(
                  alignment: Alignment.topLeft,
                  maxWidth: 400,
                  maxHeight: 400,
                  child: Padding(
                    padding: const EdgeInsets.only(left: 60),
                    child: Align(
                      alignment: Alignment.topLeft,
                      child: sticker('/a'),
                    ),
                  ),
                ),
              ),
            ),
          ),
        ),
      );
      await settle(tester);
      expect(host.created, hasLength(1));
      expect(playing(), isEmpty);
      await finish(tester);
    });

    testWidgets('a sticker off the edge of the screen does not play', (
      tester,
    ) async {
      await tester.pumpWidget(
        app(
          Stack(
            children: [
              Positioned(left: 900, top: 0, child: sticker('/a')),
              Positioned(left: 750, top: 0, child: sticker('/b')),
            ],
          ),
        ),
      );
      await settle(tester);
      expect(playing(), {2});
      await finish(tester);
    });

    testWidgets('Offstage stops playback and coming back resumes it', (
      tester,
    ) async {
      Widget build(bool offstage) => app(
        Center(
          child: Offstage(offstage: offstage, child: sticker('/a')),
        ),
      );
      await tester.pumpWidget(build(false));
      await settle(tester);
      expect(playing(), {1});
      await tester.pumpWidget(build(true));
      await settle(tester);
      expect(playing(), isEmpty);
      await tester.pumpWidget(build(false));
      await settle(tester);
      expect(playing(), {1});
      await finish(tester);
    });

    testWidgets('a shared animation plays while any copy is visible', (
      tester,
    ) async {
      Widget build(double left) => app(
        Stack(
          children: [
            Positioned(left: 0, top: 0, child: sticker('/a')),
            Positioned(left: left, top: 0, child: sticker('/a')),
          ],
        ),
      );
      await tester.pumpWidget(build(900));
      await settle(tester);
      expect(playing(), {1});
      expect(WebmStickers.debugActiveCount.value, 1);
      await tester.pumpWidget(build(200));
      await settle(tester);
      expect(playing(), {1});
      expect(WebmStickers.debugActiveCount.value, 2);
      expect(host.activeCalls, [(1, true)]);
      await finish(tester);
    });
  });

  group('TickerMode', () {
    testWidgets('a disabled TickerMode pauses, enabling resumes', (
      tester,
    ) async {
      Widget build(bool enabled) => app(
        Center(
          child: TickerMode(enabled: enabled, child: sticker('/a')),
        ),
      );
      await tester.pumpWidget(build(true));
      await settle(tester);
      expect(playing(), {1});
      await tester.pumpWidget(build(false));
      expect(playing(), isEmpty);
      await tester.pumpWidget(build(true));
      expect(playing(), {1});
      await finish(tester);
    });

    testWidgets('a sticker created under a disabled TickerMode stays paused', (
      tester,
    ) async {
      await tester.pumpWidget(
        app(Center(child: TickerMode(enabled: false, child: sticker('/a')))),
      );
      await settle(tester);
      expect(host.created, hasLength(1));
      expect(host.activeCalls, isEmpty);
      await finish(tester);
    });
  });

  group('app lifecycle', () {
    testWidgets('backgrounding pauses, resuming plays again', (tester) async {
      await tester.pumpWidget(app(Center(child: sticker('/a'))));
      await settle(tester);
      expect(playing(), {1});

      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      expect(playing(), {1}, reason: 'inactive is still on screen');
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      expect(playing(), isEmpty);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      expect(playing(), isEmpty);

      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      expect(playing(), {1});
      await finish(tester);
    });
  });

  group('playback control', () {
    testWidgets('autoplay false shows a paused, private animation', (
      tester,
    ) async {
      await tester.pumpWidget(
        app(Row(children: [sticker('/a'), sticker('/a', autoplay: false)])),
      );
      await settle(tester);
      expect(host.created, hasLength(2));
      expect(playing(), {1});
      await finish(tester);
    });

    testWidgets('the controller pauses and plays its own animation', (
      tester,
    ) async {
      final controller = WebmStickerController();
      await tester.pumpWidget(
        app(
          Row(
            children: [
              sticker('/a'),
              sticker('/a', controller: controller),
              sticker('/a', controller: controller),
            ],
          ),
        ),
      );
      await settle(tester);
      expect(host.created, hasLength(2));
      expect(playing(), {1, 2});

      controller.pause();
      expect(controller.isPlaying, isFalse);
      expect(playing(), {1});
      controller.play();
      expect(playing(), {1, 2});
      await finish(tester);
      controller.dispose();
    });

    testWidgets('a controller starting paused does not play', (tester) async {
      final controller = WebmStickerController(playing: false);
      await tester.pumpWidget(
        app(Center(child: sticker('/a', controller: controller))),
      );
      await settle(tester);
      expect(host.created, hasLength(1));
      expect(playing(), isEmpty);
      controller.play();
      expect(playing(), {1});
      await finish(tester);
      controller.dispose();
    });

    testWidgets('a controller change during the probe still loads', (
      tester,
    ) async {
      final controller = WebmStickerController();
      probeGate = Completer<void>();
      Widget build(WebmStickerController? c) =>
          app(Center(child: sticker('/a', controller: c)));
      await tester.pumpWidget(build(null));
      await tester.pump();
      await tester.pumpWidget(build(controller));
      probeGate!.complete();
      await settle(tester);
      expect(host.created, hasLength(1));
      expect(find.byType(Texture), findsOneWidget);
      expect(playing(), {1});
      await finish(tester);
      controller.dispose();
    });

    testWidgets('adding a controller moves to a private animation', (
      tester,
    ) async {
      final controller = WebmStickerController(playing: false);
      Widget build(WebmStickerController? c) =>
          app(Center(child: sticker('/a', controller: c)));
      await tester.pumpWidget(build(null));
      await settle(tester);
      expect(playing(), {1});

      await tester.pumpWidget(build(controller));
      await settle(tester);
      expect(host.created, hasLength(2));
      expect(tester.widget<Texture>(find.byType(Texture)).textureId, 102);
      expect(playing(), isEmpty);
      await tester.pump(_grace);
      expect(host.disposed, [1]);
      await finish(tester);
      controller.dispose();
    });
  });

  group('sources', () {
    late FakeFetcher fetcher;

    setUp(() {
      fetcher = FakeFetcher();
      runtime.resolver.fetcher = fetcher;
    });

    Widget networkSticker(
      String url, {
      Key? key,
      String? stickerKey,
      StickerErrorWidgetBuilder? errorBuilder,
    }) => WebmSticker(
      key: key,
      source: StickerSource.network(url, key: stickerKey),
      width: 100,
      height: 100,
      errorBuilder: errorBuilder,
    );

    testWidgets('a network sticker is fetched, then created from its bytes', (
      tester,
    ) async {
      final data = Uint8List.fromList([1, 2, 3]);
      fetcher.result = StickerFetchResult.bytes(data);
      await tester.pumpWidget(app(Center(child: networkSticker('https://a'))));
      await settle(tester);
      expect(fetcher.calls.single.$1, Uri.parse('https://a'));
      final (key, source, _, _) = host.created.single;
      expect(key, startsWith('sha1:'));
      expect(source.bytes, data);
      expect(find.byType(Texture), findsOneWidget);
      await finish(tester);
    });

    testWidgets('a fetched file is created by path', (tester) async {
      fetcher.result = const StickerFetchResult.file('/cache/a.webm');
      await tester.pumpWidget(app(Center(child: networkSticker('https://a'))));
      await settle(tester);
      expect(host.created.single.$1, 'file:/cache/a.webm');
      expect(host.created.single.$2.path, '/cache/a.webm');
      await finish(tester);
    });

    testWidgets('two widgets on one url fetch and create once', (tester) async {
      await tester.pumpWidget(
        app(
          Column(
            children: [
              networkSticker('https://a'),
              networkSticker('https://a'),
            ],
          ),
        ),
      );
      await settle(tester);
      expect(fetcher.calls, hasLength(1));
      expect(host.created, hasLength(1));
      await finish(tester);
    });

    testWidgets('different urls with the same bytes share an animation', (
      tester,
    ) async {
      await tester.pumpWidget(
        app(
          Column(
            children: [
              networkSticker('https://a'),
              networkSticker('https://b'),
            ],
          ),
        ),
      );
      await settle(tester);
      expect(fetcher.calls, hasLength(2));
      expect(host.created, hasLength(1));
      await finish(tester);
    });

    testWidgets('a fetch failure reaches errorBuilder, no platform call', (
      tester,
    ) async {
      fetcher.failWith = StateError('offline');
      await tester.pumpWidget(
        app(
          Center(
            child: networkSticker(
              'https://a',
              errorBuilder: (_, e) => Text('failed: $e'),
            ),
          ),
        ),
      );
      await settle(tester);
      expect(find.textContaining('offline'), findsOneWidget);
      expect(host.created, isEmpty);
      await finish(tester);
    });

    testWidgets('no fetcher configured shows the error', (tester) async {
      runtime.resolver.fetcher = null;
      await tester.pumpWidget(
        app(
          Center(
            child: networkSticker(
              'https://a',
              errorBuilder: (_, e) => Text('${e.runtimeType}'),
            ),
          ),
        ),
      );
      await settle(tester);
      expect(find.text('StateError'), findsOneWidget);
      await finish(tester);
    });

    testWidgets('changing the url loads the new sticker', (tester) async {
      const k = ValueKey('s');
      await tester.pumpWidget(
        app(Center(child: networkSticker('https://a', key: k))),
      );
      await settle(tester);
      fetcher.result = StickerFetchResult.bytes(Uint8List.fromList([9]));
      await tester.pumpWidget(
        app(Center(child: networkSticker('https://b', key: k))),
      );
      await settle(tester);
      expect(fetcher.calls.map((c) => c.$1.toString()), [
        'https://a',
        'https://b',
      ]);
      expect(host.created, hasLength(2));
      await finish(tester);
    });

    testWidgets('a bytes sticker is keyed by content hash', (tester) async {
      Widget bytes(List<int> data) => WebmSticker(
        source: StickerSource.bytes(Uint8List.fromList(data)),
        width: 100,
        height: 100,
      );
      await tester.pumpWidget(
        app(
          Column(
            children: [
              bytes([1, 2]),
              bytes([1, 2]),
              bytes([3]),
            ],
          ),
        ),
      );
      await settle(tester);
      // Equal copies share an animation; different content doesn't.
      expect(host.created, hasLength(2));
      await finish(tester);
    });
  });

  testWidgets('disposing releases the animation after the grace', (
    tester,
  ) async {
    await tester.pumpWidget(app(Center(child: sticker('/a'))));
    await settle(tester);
    await tester.pumpWidget(const SizedBox());
    expect(playing(), isEmpty);
    expect(host.disposed, isEmpty);
    await tester.pump(_grace);
    expect(host.disposed, [1]);
    await finish(tester);
  });

  testWidgets('unmounting before the platform answers releases the lease', (
    tester,
  ) async {
    host.gate = Completer<void>();
    await tester.pumpWidget(app(Center(child: sticker('/a'))));
    await settle(tester);
    await tester.pumpWidget(const SizedBox());
    host.gate!.complete();
    await settle(tester);
    expect(host.activeCalls, isEmpty);
    await tester.pump(_grace);
    expect(host.disposed, [1]);
    await finish(tester);
  });
}
