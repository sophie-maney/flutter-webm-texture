import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'webm_texture_method_channel.dart';

abstract class WebmTexturePlatform extends PlatformInterface {
  /// Constructs a WebmTexturePlatform.
  WebmTexturePlatform() : super(token: _token);

  static final Object _token = Object();

  static WebmTexturePlatform _instance = MethodChannelWebmTexture();

  /// The default instance of [WebmTexturePlatform] to use.
  ///
  /// Defaults to [MethodChannelWebmTexture].
  static WebmTexturePlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [WebmTexturePlatform] when
  /// they register themselves.
  static set instance(WebmTexturePlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
