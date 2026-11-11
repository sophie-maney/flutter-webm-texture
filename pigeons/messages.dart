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

  /// Shows [frame] (0-based) right away, whether or not the animation is
  /// active; an active one carries on from there. Seeking a non-looping
  /// animation that has ended lets it play again.
  ///
  /// Throws `WT_ERR_INVALID_ARGUMENT` when [frame] is outside the
  /// animation's frames. An unknown or disposed [animId] is ignored.
  void seekToFrame(int animId, int frame);

  /// Releases the animation, its texture and its decoder. Safe to call twice.
  void dispose(int animId);

  /// Reads the current device signals.
  DeviceSignalsMessage deviceSignals();

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
}
