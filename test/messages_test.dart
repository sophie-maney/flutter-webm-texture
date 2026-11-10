import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/gen/messages.g.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const channel =
      'dev.flutter.pigeon.webm_texture.WebmTextureHostApi.createAnimation';

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMessageHandler(channel, null);
  });

  test('createAnimation round-trips its arguments and result', () async {
    List<Object?>? received;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMessageHandler(channel, (ByteData? message) async {
          received = WebmTextureHostApi.pigeonChannelCodec.decodeMessage(
            message,
          ) as List<Object?>;
          return WebmTextureHostApi.pigeonChannelCodec.encodeMessage(<Object?>[
            AnimationHandleMessage(animId: 7, textureId: 9),
          ]);
        });

    final handle = await WebmTextureHostApi().createAnimation(
      'key',
      AnimationSourceMessage(path: '/tmp/a.webm'),
      128,
      96,
    );

    expect(handle.animId, 7);
    expect(handle.textureId, 9);
    expect(received![0], 'key');
    expect((received![1]! as AnimationSourceMessage).path, '/tmp/a.webm');
    expect((received![1]! as AnimationSourceMessage).bytes, isNull);
    expect(received!.sublist(2), [128, 96]);
  });

  test('bytes sources and signals survive the codec', () {
    final codec = WebmTextureHostApi.pigeonChannelCodec;
    final source =
        codec.decodeMessage(
              codec.encodeMessage(
                AnimationSourceMessage(bytes: Uint8List.fromList([1, 2, 3])),
              ),
            )!
            as AnimationSourceMessage;
    expect(source.bytes, [1, 2, 3]);

    final signals =
        codec.decodeMessage(
              codec.encodeMessage(
                DeviceSignalsMessage(
                  ramMb: 3072,
                  cores: 8,
                  lowRam: false,
                  perfClass: PerfClassMessage.mid,
                  lowPower: true,
                  thermal: ThermalStateMessage.serious,
                ),
              ),
            )!
            as DeviceSignalsMessage;
    expect(signals.perfClass, PerfClassMessage.mid);
    expect(signals.thermal, ThermalStateMessage.serious);
    expect(signals.lowPower, isTrue);
  });
}
