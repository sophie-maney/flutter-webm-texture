import Flutter
import UIKit

import wt_core
import wt_ios

public class WebmTexturePlugin: NSObject, FlutterPlugin, WebmTextureHostApi {
  /// Dart finds the core with DynamicLibrary.process(). The app is statically
  /// linked, so reference every entry point from Swift, or the linker would
  /// strip the ones nothing else uses.
  private static var keepAlive: [Any] = []

  private let registry: FlutterTextureRegistry
  private let errors = ErrorStream()
  private let signals = SignalStream()

  // Main thread only.
  private var animations: [Int64: (texture: StickerTexture, textureId: Int64)] = [:]
  private var lastAnimId: Int64 = 0
  private var detached = false
  /// Where the frame cache lives; created when the first animation needs it.
  private var cacheDirectory: String

  init(registry: FlutterTextureRegistry) {
    self.registry = registry
    cacheDirectory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("webm_texture", isDirectory: true).path
    super.init()
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    keepAlive = [
      wt_api_version, wt_status_string, wt_limits_init_default,
      wt_source_create_memory, wt_source_create_file, wt_source_destroy,
      wt_probe, wt_anim_create, wt_anim_destroy, wt_anim_get_info, wt_anim_seek,
      wt_anim_next_frame, wt_anim_create_cached, wt_cache_set_budget, wt_cache_get_budget,
      wt_cache_file_path, wt_cache_get_stats, wt_cache_evict, wt_cache_clear,
      wt_sched_options_init_default, wt_sched_create, wt_sched_destroy, wt_sched_set_options,
      wt_sched_get_stats, wt_player_create, wt_player_destroy, wt_player_set_active,
      wt_player_set_priority, wt_player_seek, wt_player_redraw,
    ]
    let channel = FlutterMethodChannel(name: "webm_texture", binaryMessenger: registrar.messenger())
    let instance = WebmTexturePlugin(registry: registrar.textures())
    // Keep the cache within its budget from the start; frame writes keep it
    // there afterwards.
    let startupDirectory = instance.cacheDirectory
    DispatchQueue.global(qos: .utility).async {
      _ = WTPlayer.evictCache(inDirectory: startupDirectory)
    }
    registrar.addMethodCallDelegate(instance, channel: channel)
    // Publishing makes the engine call detachFromEngine(for:) on teardown.
    registrar.publish(instance)
    WebmTextureHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: instance)
    StreamErrorsStreamHandler.register(with: registrar.messenger(), streamHandler: instance.errors)
    StreamDeviceSignalsStreamHandler.register(
      with: registrar.messenger(), streamHandler: instance.signals)
  }

  public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
    detached = true
    for animId in Array(animations.keys) { disposeNow(animId) }
    WebmTextureHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: nil)
  }

  // MARK: WebmTextureHostApi

  func createAnimation(
    sourceKey: String, source: AnimationSourceMessage, bucketW: Int64, bucketH: Int64,
    loop: Bool
  ) async throws -> AnimationHandleMessage {
    let (animId, cacheDirectory) = await MainActor.run { () -> (Int64, String) in
      lastAnimId += 1
      return (lastAnimId, self.cacheDirectory)
    }
    // Probes the source off the main thread; the scheduler draws frame 0.
    let texture = try await StickerTexture.create(
      animId: animId, source: source, sourceKey: sourceKey, bucketW: bucketW, bucketH: bucketH,
      loop: loop, cacheDirectory: cacheDirectory, registry: registry,
      onError: { [plugin = WeakPlugin(self)] animId, code, message in
        DispatchQueue.main.async {
          plugin.value?.emitError(animId: animId, code: code, message: message)
        }
      })
    return try await MainActor.run {
      guard !detached else {
        texture.close()
        throw PigeonError(
          code: "ENGINE_DETACHED", message: "the Flutter engine is shutting down", details: nil)
      }
      // The engine expects texture registration on the platform thread.
      let textureId = registry.register(texture)
      texture.attach(textureId: textureId)
      animations[animId] = (texture, textureId)
      return AnimationHandleMessage(animId: animId, textureId: textureId)
    }
  }

  func setActive(animId: Int64, active: Bool) throws {
    // An id that was already disposed is ignored: Dart may race a dispose
    // with a visibility change.
    animations[animId]?.texture.setActive(active)
  }

  func setPriority(animId: Int64, priority: Int64) throws {
    animations[animId]?.texture.setPriority(priority)
  }

  func seekToFrame(animId: Int64, frame: Int64) throws {
    guard let texture = animations[animId]?.texture else { return }
    guard frame >= 0, frame < Int64(texture.frameCount) else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT",
        message: "frame \(frame) is outside 0..<\(texture.frameCount)", details: nil)
    }
    texture.seek(toFrame: Int(frame))
  }

  func dispose(animId: Int64) throws {
    disposeNow(animId)
  }

  func configureCache(directory: String?) throws -> String {
    guard let directory else { return cacheDirectory }
    guard directory.hasPrefix("/") else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT",
        message: "the cache directory must be absolute: \(directory)", details: nil)
    }
    cacheDirectory = directory
    return directory
  }

  func deviceSignals() throws -> DeviceSignalsMessage {
    SignalStream.read()
  }

  func setSchedulerOptions(workerCount: Int64, maxFps: Int64, maxConcurrent: Int64) throws {
    let range = 0...Int64(Int32.max)
    let status =
      range.contains(workerCount) && range.contains(maxFps) && range.contains(maxConcurrent)
      ? WTPlayer.setSchedulerOptions(
        workerCount: Int32(workerCount), maxFps: Int32(maxFps), maxConcurrent: Int32(maxConcurrent))
      : Int32(WT_ERR_INVALID_ARGUMENT)
    if status != Int32(WT_OK) {
      throw PigeonError(
        code: String(cString: wt_status_string(status)),
        message: "can't apply the scheduler options", details: nil)
    }
  }

  // MARK: Internals (main thread)

  private func disposeNow(_ animId: Int64) {
    guard let entry = animations.removeValue(forKey: animId) else { return }
    // Unregister first so Flutter stops sampling, then free the decoder and
    // buffers once any in-flight frame finishes (off the main thread).
    registry.unregisterTexture(entry.textureId)
    entry.texture.close()
  }

  private func emitError(animId: Int64, code: String, message: String) {
    // Only report animations that are still alive.
    guard animations[animId] != nil else { return }
    errors.send(AnimationErrorMessage(animId: animId, code: code, message: message))
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "getPlatformVersion":
      result("iOS " + UIDevice.current.systemVersion)
    default:
      result(FlutterMethodNotImplemented)
    }
  }
}

/// A weak reference to the plugin that worker queues can carry. It is only
/// dereferenced on the main thread, which owns all of the plugin's state.
private final class WeakPlugin: @unchecked Sendable {
  weak var value: WebmTexturePlugin?
  init(_ value: WebmTexturePlugin) { self.value = value }
}

/// Holds the Dart listener of `streamErrors`, if any. Main thread only:
/// Flutter event sinks must be called on the platform thread.
private final class ErrorStream: StreamErrorsStreamHandler {
  private var sink: PigeonEventSink<AnimationErrorMessage>?

  override func onListen(withArguments arguments: Any?, sink: PigeonEventSink<AnimationErrorMessage>) {
    self.sink = sink
  }

  override func onCancel(withArguments arguments: Any?) {
    sink = nil
  }

  func send(_ error: AnimationErrorMessage) {
    sink?.success(error)
  }
}

/// Reports Low Power Mode and thermal changes to Dart while it listens. Main
/// thread only, like every Flutter event sink.
private final class SignalStream: StreamDeviceSignalsStreamHandler {
  private var observers: [NSObjectProtocol] = []

  static func read() -> DeviceSignalsMessage {
    let info = ProcessInfo.processInfo
    let thermal: ThermalStateMessage
    switch info.thermalState {
    case .nominal: thermal = .nominal
    case .fair: thermal = .fair
    case .serious: thermal = .serious
    case .critical: thermal = .critical
    @unknown default: thermal = .unknown
    }
    return DeviceSignalsMessage(
      ramMb: Int64(info.physicalMemory / (1024 * 1024)),
      // processorCount, not activeProcessorCount: the latter can drop in Low
      // Power Mode, which the tier already counts separately.
      cores: Int64(info.processorCount),
      lowRam: false,
      perfClass: .unknown,
      lowPower: info.isLowPowerModeEnabled,
      thermal: thermal)
  }

  override func onListen(withArguments arguments: Any?, sink: PigeonEventSink<DeviceSignalsMessage>) {
    stop()
    let notify: (Notification) -> Void = { _ in sink.success(SignalStream.read()) }
    let center = NotificationCenter.default
    observers = [
      center.addObserver(
        forName: .NSProcessInfoPowerStateDidChange, object: nil, queue: .main, using: notify),
      center.addObserver(
        forName: ProcessInfo.thermalStateDidChangeNotification, object: nil, queue: .main,
        using: notify),
    ]
  }

  override func onCancel(withArguments arguments: Any?) {
    stop()
  }

  private func stop() {
    observers.forEach(NotificationCenter.default.removeObserver)
    observers = []
  }
}
