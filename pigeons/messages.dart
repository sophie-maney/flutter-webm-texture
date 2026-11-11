// Pigeon definition of the platform channel API (texture lifecycle, device
// signals, errors). Media logic never crosses this boundary: the core is
// reached over dart:ffi (ADR 0009).
//
// Regenerate with: fvm dart run pigeon --input pigeons/messages.dart
// ignore_for_file: one_member_abstracts
import 'package:pigeon/pigeon.dart';

@ConfigurePigeon(
  PigeonOptions(
    dartOut: 'lib/src/gen/messages.g.dart',
    dartOptions: DartOptions(),
    kotlinOut: 'android/src/main/kotlin/com/example/webm_texture/Messages.g.kt',
    kotlinOptions: KotlinOptions(package: 'com.example.webm_texture'),
    swiftOut: 'ios/webm_texture/Sources/webm_texture/Messages.g.swift',
    swiftOptions: SwiftOptions(),
    dartPackageName: 'webm_texture',
  ),
)
/// Where the sticker bytes come from. Exactly one of [bytes] and [path] is set.
class AnimationSourceMessage {
  AnimationSourceMessage({this.bytes, this.path});

  /// The encoded file contents.
  Uint8List? bytes;

  /// An absolute path to the encoded file.
  String? path;
}

/// What [WebmTextureHostApi.createAnimation] returns.
class AnimationHandleMessage {
  AnimationHandleMessage({required this.animId, required this.textureId});

  /// Identifies the animation in every later call.
  int animId;

  /// The Flutter texture id to give to a `Texture` widget.
  int textureId;
}

/// How capable the device is at sustained media-performance work.
enum PerfClassMessage {
  /// The platform doesn't say (iOS, or Android below API 31).
  unknown,
  low,
  mid,
  high,
}

/// Thermal pressure, from `PowerManager.getCurrentThermalStatus` (Android)
/// or `ProcessInfo.thermalState` (iOS).
enum ThermalStateMessage {
  /// Not reported (Android below API 29).
  unknown,
  nominal,
  fair,
  serious,
  critical,
}

/// Inputs to tier detection (PLAN.md "Tiers"). [lowPower] and [thermal] can
/// change at runtime, so Dart polls this when the app resumes.
class DeviceSignalsMessage {
  DeviceSignalsMessage({
    required this.ramMb,
    required this.cores,
    required this.lowRam,
    required this.perfClass,
    required this.lowPower,
    required this.thermal,
  });

  /// Physical memory in MiB.
  int ramMb;

  /// Logical CPU cores.
  int cores;

  /// Android `isLowRamDevice`; always false on iOS.
  bool lowRam;

  /// Android media performance class; [PerfClassMessage.unknown] on iOS.
  PerfClassMessage perfClass;

  /// iOS Low Power Mode or Android battery saver.
  bool lowPower;

  /// Current thermal pressure.
  ThermalStateMessage thermal;
}

/// An asynchronous failure from a platform animation, delivered on
/// [streamErrors].
class AnimationErrorMessage {
  AnimationErrorMessage({
    required this.animId,
    required this.code,
    required this.message,
  });

  /// The animation that failed.
  int animId;

  /// A stable identifier, such as a `wt_status` name (`WT_ERR_LIMIT`) or
  /// `SURFACE_LOST`.
  String code;

  /// A human-readable description.
  String message;
}

/// Layout of an atlas's pages ([WebmTextureHostApi.createAtlas]; see
/// `wt_atlas_options` in `wt_api.h`).
class AtlasOptionsMessage {
  AtlasOptionsMessage({
    required this.width,
    required this.maxHeight,
    required this.maxPages,
  });

  /// Page width in physical pixels, 16..2048 (rounded down to 16).
  int width;

  /// The tallest a page may grow, 16..2048 (rounded down to 16).
  int maxHeight;

  /// The most pages (platform textures) the atlas may use, 1..16.
  int maxPages;
}

/// Where an animation attached to an atlas is drawn, in its page's pixels
/// (origin top left). Each slot has a transparent gutter of at least 2 px
/// around this rect.
class AtlasSlotMessage {
  AtlasSlotMessage({
    required this.animId,
    required this.pageId,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
    this.error,
  });

  int animId;

  /// The page; draw [AtlasPageMessage.textureId] of this page. -1 when
  /// [error] is set.
  int pageId;

  /// The animation's frame, at its bucket size.
  int x;
  int y;
  int width;
  int height;

  /// Null on success. Otherwise a `wt_status` name, such as `WT_ERR_LIMIT`
  /// (every page is full) or `WT_ERR_INVALID_ARGUMENT` (unknown animation,
  /// already in an atlas, or too big for a page): the animation keeps its
  /// own texture.
  String? error;
}

/// An atlas page as Dart should draw it: a `Texture(textureId)` laid out at
/// [width] x [height] physical pixels, clipped to a slot.
class AtlasPageMessage {
  AtlasPageMessage({
    required this.atlasId,
    required this.pageId,
    required this.textureId,
    required this.width,
    required this.height,
  });

  int atlasId;
  int pageId;
  int textureId;
  int width;
  int height;
}

/// What a call that moves animations into or out of an atlas changed.
class AtlasLayoutMessage {
  AtlasLayoutMessage({
    required this.slots,
    required this.handles,
    required this.pages,
    required this.retiredTextureIds,
  });

  /// [WebmTextureHostApi.attachToAtlas]: one per animation, in order.
  List<AtlasSlotMessage> slots;

  /// [WebmTextureHostApi.detachFromAtlas]: each animation's new texture of
  /// its own, in order (textureId -1 for an unknown animation).
  List<AnimationHandleMessage> handles;

  /// Every page of the atlases involved that has a texture, after the call.
  /// A page whose size changed has a new texture id: slots on it are drawn
  /// with the new id and size from now on.
  List<AtlasPageMessage> pages;

  /// Textures this call replaced (a page's previous size, an animation's own
  /// texture when it entered the atlas). They hold their last frame until
  /// Dart calls [WebmTextureHostApi.releaseTextures], which it does once no
  /// widget draws them any more.
  List<int> retiredTextureIds;
}

/// One atlas page's state and memory ([WebmTextureHostApi.atlasStats]).
class AtlasPageStatsMessage {
  AtlasPageStatsMessage({
    required this.pageId,
    required this.textureId,
    required this.width,
    required this.height,
    required this.usedHeight,
    required this.liveSlots,
    required this.buffers,
    required this.bufferBytes,
    required this.cpuBytes,
    required this.presents,
    required this.presentFailures,
  });

  int pageId;

  /// -1 when the page has no texture (it is empty).
  int textureId;
  int width;

  /// The texture's height; 0 without one.
  int height;

  /// Rows the slots use.
  int usedHeight;
  int liveSlots;

  /// Distinct platform buffers the page's queue handed out (gralloc buffers
  /// on Android, IOSurfaces on iOS).
  int buffers;

  /// [buffers] x row stride x [height]: the page's graphics memory.
  int bufferBytes;

  /// The slots' latest frames and spare decode buffers, in RAM.
  int cpuBytes;

  /// Frames shown (one per scheduler tick at most) and failed attempts.
  int presents;
  int presentFailures;
}

/// [WebmTextureHostApi.atlasStats].
class AtlasStatsMessage {
  AtlasStatsMessage({
    required this.pages,
    required this.textures,
    required this.pluginTextures,
  });

  List<AtlasPageStatsMessage> pages;

  /// Platform textures the plugin holds for this atlas: its pages' current
  /// textures.
  int textures;

  /// Platform textures the plugin holds in all: animations' own, every
  /// atlas page's, and retired ones not released yet.
  int pluginTextures;
}

/// Texture lifecycle and device queries. Handlers run on the platform main
/// thread and must not decode; decoding happens on native worker threads.
@HostApi()
abstract class WebmTextureHostApi {
  /// Creates the decoder and a platform texture sized to the bucket
  /// ([bucketW] x [bucketH] physical pixels). The texture shows frame 0 and
  /// stays inactive until [setActive].
  ///
  /// [sourceKey] is the content hash (or caller key) that names the cache
  /// file (`wt_cache_file_path`, under [configureCache]'s directory), unless
  /// the bucket is 0 x 0, whose output size isn't known yet: such an
  /// animation isn't cached. With [loop] false, playback stops on the last frame and holds it
  /// until [seekToFrame]. Fails with a `PlatformException` carrying a
  /// `wt_status` name as its code if the core rejects the source.
  @async
  AnimationHandleMessage createAnimation(
    String sourceKey,
    AnimationSourceMessage source,
    int bucketW,
    int bucketH,
    bool loop,
  );

  /// Starts or stops playback. Inactive animations keep their current frame.
  void setActive(int animId, bool active);

  /// Ranks the animation for the native scheduler's `maxConcurrent` limit
  /// (ADR 0017): [priority] is the area, in physical pixels, that the
  /// widgets playing it show on screen. When more animations are active than
  /// the limit allows, the largest visible areas play, then the most recently
  /// activated; the rest hold their frame. Starts at 0. An unknown or
  /// disposed [animId] is ignored.
  void setPriority(int animId, int priority);

  /// Shows [frame] (0-based) right away, whether or not the animation is
  /// active; an active one carries on from there. Seeking a non-looping
  /// animation that has ended lets it play again.
  ///
  /// Throws `WT_ERR_INVALID_ARGUMENT` when [frame] is outside the
  /// animation's frames. An unknown or disposed [animId] is ignored.
  void seekToFrame(int animId, int frame);

  /// Releases the animation, its texture and its decoder. Safe to call twice.
  /// An animation in an atlas leaves it; its page keeps its size until the
  /// atlas's next attach or detach.
  void dispose(int animId);

  /// Reads the current device signals.
  DeviceSignalsMessage deviceSignals();

  /// Applies the tier's scheduler tunables (`wt_sched_set_options`, ADR 0018)
  /// to the process-wide scheduler, creating it if needed. [workerCount] is
  /// 1..8, [maxFps] is 1..240, and a [maxConcurrent] of 0 means no limit.
  /// Changes reach running animations on the next scheduler round. Throws
  /// `WT_ERR_INVALID_ARGUMENT` for a value out of range.
  void setSchedulerOptions(int workerCount, int maxFps, int maxConcurrent);

  /// Creates an empty atlas (ADR 0020): a set of pages, each one platform
  /// texture that many animations draw into. No texture exists until an
  /// animation is attached.
  int createAtlas(AtlasOptionsMessage options);

  /// Moves animations into atlas [atlasId], each into a slot of its bucket
  /// size on a page with room (adding a page when needed), and retires their
  /// own textures. An animation keeps playing, pausing, seeking and
  /// reporting errors as before. Pages that grew get a new texture.
  AtlasLayoutMessage attachToAtlas(int atlasId, List<int> animIds);

  /// Moves animations out of the atlas they are in, onto a new texture of
  /// their own (also for an animation whose atlas was disposed). Pages that
  /// shrank get a new texture; a page left empty has none.
  AtlasLayoutMessage detachFromAtlas(List<int> animIds);

  /// Releases retired textures (see [AtlasLayoutMessage.retiredTextureIds]).
  /// Unknown ids are ignored.
  void releaseTextures(List<int> textureIds);

  /// For each of [animIds], the page texture that shows its slot's frame,
  /// or -1: before a present has written the slot's first frame into the
  /// page's current texture (so right after [attachToAtlas], and for every
  /// slot of a page that just got a new texture), and for an animation that
  /// isn't in atlas [atlasId]. Until then the slot's area of the texture
  /// holds stale pixels: keep showing the placeholder, or the texture shown
  /// before (`wt_atlas_shown_page`).
  List<int> atlasShownTextures(int atlasId, List<int> animIds);

  /// The atlas's pages and memory.
  AtlasStatsMessage atlasStats(int atlasId);

  /// Frees the atlas. Animations still in it are left without a texture
  /// until [detachFromAtlas] (or [dispose]). Its page textures are retired,
  /// as listed in the result.
  AtlasLayoutMessage disposeAtlas(int atlasId);

  /// Sets the directory for the frame cache and returns the one in use, an
  /// absolute path. With a null [directory] nothing changes: the result is
  /// the current one, which starts as `webm_texture` under the app's cache
  /// directory (`Context.cacheDir` on Android, `Library/Caches` on iOS).
  ///
  /// The directory is created when the first animation needs it, and only
  /// ever holds files the core wrote (`*.wsc`). Animations created
  /// afterwards cache there; existing ones keep their files. Cache
  /// administration (`wt_cache_*`) runs over FFI, so Dart passes this path to
  /// it.
  String configureCache(String? directory);
}

/// Errors that happen after `createAnimation` returned, such as a lost
/// surface or a decode failure mid-playback.
@EventChannelApi()
abstract class WebmTextureEvents {
  AnimationErrorMessage streamErrors();

  /// A fresh reading of every device signal, sent when Low Power Mode, battery
  /// saver or the thermal state changes while the app runs. Same content as
  /// [WebmTextureHostApi.deviceSignals].
  DeviceSignalsMessage streamDeviceSignals();
}
