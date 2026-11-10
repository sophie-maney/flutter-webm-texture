// The example against a fake platform: the landing page, and the Textures
// page giving every playable sticker a Texture and the oversized one its
// error code.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
// ignore: implementation_imports
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture_example/main.dart';
import 'package:webm_texture_example/texture_demo.dart';

const _prefix = 'dev.flutter.pigeon.webm_texture.WebmTextureHostApi';

void main() {
  testWidgets('Verify Platform version', (tester) async {
    await tester.pumpWidget(const MyApp());
    expect(
      find.byWidgetPredicate(
        (widget) => widget is Text && widget.data!.startsWith('Running on:'),
      ),
      findsOneWidget,
    );
    expect(find.byKey(const Key('openTextures')), findsOneWidget);
  });

  testWidgets('shows a texture per playable sticker', (tester) async {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const codec = WebmTextureHostApi.pigeonChannelCodec;
    var nextId = 1;
    final active = <int, bool>{};
    final disposed = <int>{};

    messenger.setMockDecodedMessageHandler<Object?>(
      const BasicMessageChannel('$_prefix.createAnimation', codec),
      (message) async {
        final key = (message! as List<Object?>)[0]! as String;
        if (key == 'vp9_2048.webm') return ['WT_ERR_LIMIT', 'too big', null];
        if (key == 'garbage.bin') {
          return ['WT_ERR_INVALID_DATA', 'not a sticker', null];
        }
        final id = nextId++;
        return [AnimationHandleMessage(animId: id, textureId: id + 100)];
      },
    );
    messenger.setMockDecodedMessageHandler<Object?>(
      const BasicMessageChannel('$_prefix.setActive', codec),
      (message) async {
        final args = message! as List<Object?>;
        active[args[0]! as int] = args[1]! as bool;
        return [null];
      },
    );
    messenger.setMockDecodedMessageHandler<Object?>(
      const BasicMessageChannel('$_prefix.dispose', codec),
      (message) async {
        disposed.add((message! as List<Object?>)[0]! as int);
        return [null];
      },
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel(
        'dev.flutter.pigeon.webm_texture.WebmTextureEvents.streamErrors',
        pigeonMethodCodec,
      ),
      (call) async => null,
    );

    await tester.pumpWidget(const MaterialApp(home: TextureDemoPage()));
    // Asset loads, temp-file writes and channel replies are real async work,
    // and each one resumes in the fake-async zone, so alternate the two.
    for (var i = 0; i < 50 && disposed.isEmpty; i++) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 20)),
      );
      await tester.pump();
    }
    await tester.pumpAndSettle();

    expect(find.byType(Texture), findsNWidgets(7));
    expect(find.text('WT_ERR_LIMIT'), findsOneWidget);
    expect(find.text('garbage.bin: WT_ERR_INVALID_DATA'), findsOneWidget);
    // truncated.webm plays its complete frames; it is created and disposed.
    expect(disposed, hasLength(1));
    expect(active.values, everyElement(isTrue));

    await tester.tap(find.byKey(const Key('play')));
    await tester.pumpAndSettle();
    expect(active.values, everyElement(isFalse));
  });
}
