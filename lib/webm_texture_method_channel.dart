import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'webm_texture_platform_interface.dart';

/// An implementation of [WebmTexturePlatform] that uses method channels.
class MethodChannelWebmTexture extends WebmTexturePlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('webm_texture');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>(
      'getPlatformVersion',
    );
    return version;
  }
}
