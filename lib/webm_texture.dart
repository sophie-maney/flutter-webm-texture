import 'webm_texture_platform_interface.dart';

class WebmTexture {
  Future<String?> getPlatformVersion() {
    return WebmTexturePlatform.instance.getPlatformVersion();
  }
}
