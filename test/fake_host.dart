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
  Future<void> dispose(int animId) async {
    disposed.add(animId);
    slots.remove(animId);
  }

  final List<(int, bool)> activeCalls = [];

  @override
  Future<void> setActive(int animId, bool active) async =>
      activeCalls.add((animId, active));

  final List<(int, int)> priorityCalls = [];

  @override
  Future<void> setPriority(int animId, int priority) async =>
      priorityCalls.add((animId, priority));

  final List<(int, int)> seekCalls = [];

  @override
  Future<void> seekToFrame(int animId, int frame) async =>
      seekCalls.add((animId, frame));

  /// What [deviceSignals] answers with; null makes it fail like a platform
  /// that doesn't report them.
  DeviceSignalsMessage? signals = DeviceSignalsMessage(
    ramMb: 8192,
    cores: 8,
    lowRam: false,
    perfClass: PerfClassMessage.unknown,
    lowPower: false,
    thermal: ThermalStateMessage.nominal,
  );

  @override
  Future<DeviceSignalsMessage> deviceSignals() async =>
      signals ?? (throw StateError('no signals'));

  /// The arguments of each [setSchedulerOptions] call.
  final List<(int, int, int)> schedulerCalls = [];

  @override
  Future<void> setSchedulerOptions(
    int workerCount,
    int maxFps,
    int maxConcurrent,
  ) async => schedulerCalls.add((workerCount, maxFps, maxConcurrent));

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

  // --- Atlas -----------------------------------------------------------------

  /// Atlas ids created, in order.
  final List<int> atlasesCreated = [];

  /// The `animIds` of each [attachToAtlas] call.
  final List<List<int>> attachCalls = [];

  /// The `animIds` of each [atlasShownTextures] call.
  final List<List<int>> shownCalls = [];

  /// Every id passed to [releaseTextures], in order.
  final List<int> released = [];
  final List<int> atlasesDisposed = [];

  /// Slots handed out, by animation.
  final Map<int, AtlasSlotMessage> slots = {};

  /// The one page (page 0) and its texture.
  AtlasPageMessage? page;

  /// When set, [atlasShownTextures] reports only these page textures as
  /// showing their slots; null shows every slot at once.
  Set<int>? shownTextures;

  /// Slots past this many get `WT_ERR_LIMIT`.
  int maxSlots = 1 << 30;

  /// Gives the page a new texture (a new size) at the next attach.
  bool resizeNext = false;

  /// Completes the next [attachToAtlas] calls when set.
  Completer<void>? attachGate;

  int _nextTexture = 1000;

  @override
  Future<int> createAtlas(AtlasOptionsMessage options) async {
    final id = atlasesCreated.length + 1;
    atlasesCreated.add(id);
    return id;
  }

  @override
  Future<AtlasLayoutMessage> attachToAtlas(
    int atlasId,
    List<int> animIds,
  ) async {
    attachCalls.add([...animIds]);
    await attachGate?.future;
    final retired = <int>[];
    final out = <AtlasSlotMessage>[];
    for (final id in animIds) {
      if (disposed.contains(id) || slots.length >= maxSlots) {
        out.add(
          AtlasSlotMessage(
            animId: id,
            pageId: -1,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
            error: disposed.contains(id)
                ? 'WT_ERR_INVALID_ARGUMENT'
                : 'WT_ERR_LIMIT',
          ),
        );
        continue;
      }
      final i = slots.length;
      final slot = AtlasSlotMessage(
        animId: id,
        pageId: 0,
        x: (i % 8) * 144 + 2,
        y: (i ~/ 8) * 144 + 2,
        width: 128,
        height: 128,
      );
      slots[id] = slot;
      out.add(slot);
      retired.add(id + 100); // Its own texture.
    }
    final rows = (slots.length + 7) ~/ 8;
    final height = rows * 144;
    final old = page;
    if (slots.isEmpty) {
      if (old != null) retired.add(old.textureId);
      page = null;
    } else if (old == null || resizeNext || old.height < height) {
      resizeNext = false;
      if (old != null) retired.add(old.textureId);
      page = AtlasPageMessage(
        atlasId: atlasId,
        pageId: 0,
        textureId: _nextTexture++,
        width: 2048,
        height: height,
      );
    }
    return AtlasLayoutMessage(
      slots: out,
      handles: [],
      pages: [?page],
      retiredTextureIds: retired,
    );
  }

  @override
  Future<List<int>> atlasShownTextures(int atlasId, List<int> animIds) async {
    shownCalls.add([...animIds]);
    final p = page;
    return [
      for (final id in animIds)
        if (p != null &&
            slots.containsKey(id) &&
            (shownTextures?.contains(p.textureId) ?? true))
          p.textureId
        else
          -1,
    ];
  }

  @override
  Future<void> releaseTextures(List<int> textureIds) async =>
      released.addAll(textureIds);

  @override
  Future<AtlasLayoutMessage> disposeAtlas(int atlasId) async {
    atlasesDisposed.add(atlasId);
    final p = page;
    page = null;
    return AtlasLayoutMessage(
      slots: [],
      handles: [],
      pages: [],
      retiredTextureIds: [?p?.textureId],
    );
  }
}
