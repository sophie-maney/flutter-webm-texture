import 'dart:async';

import 'package:webm_texture/src/gen/messages.g.dart';

/// Records host API calls instead of reaching a platform.
class FakeHost extends WebmTextureHostApi {
  final List<(String, AnimationSourceMessage, int, int)> created = [];
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
  ) async {
    created.add((sourceKey, source, bucketW, bucketH));
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
}
