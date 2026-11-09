import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/webm_texture.dart';
import 'package:webm_texture/webm_texture_platform_interface.dart';
import 'package:webm_texture/webm_texture_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockWebmTexturePlatform
    with MockPlatformInterfaceMixin
    implements WebmTexturePlatform {
  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final WebmTexturePlatform initialPlatform = WebmTexturePlatform.instance;

  test('$MethodChannelWebmTexture is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelWebmTexture>());
  });

  test('getPlatformVersion', () async {
    WebmTexture webmTexturePlugin = WebmTexture();
    MockWebmTexturePlatform fakePlatform = MockWebmTexturePlatform();
    WebmTexturePlatform.instance = fakePlatform;

    expect(await webmTexturePlugin.getPlatformVersion(), '42');
  });
}
