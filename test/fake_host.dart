import 'dart:async';

import 'package:webm_texture/src/gen/messages.g.dart';

/// Records host API calls instead of reaching a platform.
class FakeHost extends WebmTextureHostApi {
  final List<(String, AnimationSourceMessage, int, int)> created = [];

  /// The `loop` argument of each [created] call.
  final List<bool> loops = [];
  final List<int> disposed = [];
  int _next = 1;
  Completer<void>? gate;
  Object? failWith;

  @override
  Future<AnimationHandleMessage> createAnimation(
    String sourceKey,
    AnimationSourceMessage source,
    int bucketW,
    int bucketH,
    bool loop,
  ) async {
    created.add((sourceKey, source, bucketW, bucketH));
    loops.add(loop);
    await gate?.future;
    if (failWith != null) throw failWith!;
    final id = _next++;
    return AnimationHandleMessage(animId: id, textureId: id + 100);
  }

  @override
  Future<void> dispose(int animId) async => disposed.add(animId);

  final List<(int, bool)> activeCalls = [];

  @override
  Future<void> setActive(int animId, bool active) async =>
      activeCalls.add((animId, active));

  final List<(int, int)> seekCalls = [];

  @override
  Future<void> seekToFrame(int animId, int frame) async =>
      seekCalls.add((animId, frame));

  /// What [configureCache] answers with.
  String cacheDirectory = '/fake/cache';

  /// The `directory` argument of each [configureCache] call.
  final List<String?> cacheConfigs = [];

  @override
  Future<String> configureCache(String? directory) async {
    cacheConfigs.add(directory);
    if (directory != null) cacheDirectory = directory;
    return cacheDirectory;
  }
}
