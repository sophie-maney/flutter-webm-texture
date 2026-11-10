// The demo page against a fake platform: every sticker but the oversized one
// gets a Texture, and the rejected one shows its error code.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
// ignore: implementation_imports
import 'package:webm_texture/src/gen/messages.g.dart';
import 'package:webm_texture_example/main.dart';

const _prefix = 'dev.flutter.pigeon.webm_texture.WebmTextureHostApi';

void main() {
  testWidgets('shows a texture per playable sticker', (tester) async {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const codec = WebmTextureHostApi.pigeonChannelCodec;
    var nextId = 1;
    final active = <int, bool>{};

    messenger.setMockDecodedMessageHandler<Object?>(
      const BasicMessageChannel('$_prefix.createAnimation', codec),
      (message) async {
        final key = (message! as List<Object?>)[0]! as String;
        if (key == 'vp9_2048.webm') return ['WT_ERR_LIMIT', 'too big', null];
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
      (message) async => [null],
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel(
        'dev.flutter.pigeon.webm_texture.WebmTextureEvents.streamErrors',
        pigeonMethodCodec,
      ),
      (call) async => null,
    );

    await tester.pumpWidget(const MaterialApp(home: DemoPage()));
    // Asset loads and channel replies are real async work.
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(seconds: 1)),
    );
    await tester.pumpAndSettle();

    expect(find.byType(Texture), findsNWidgets(7));
    expect(find.text('WT_ERR_LIMIT'), findsOneWidget);
    expect(active.values, everyElement(isTrue));

    await tester.tap(find.byKey(const Key('play')));
    await tester.pumpAndSettle();
    expect(active.values, everyElement(isFalse));
  });
}
