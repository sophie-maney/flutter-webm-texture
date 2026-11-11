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

  /// An animation and where it is shown: its own texture, or an atlas.
  private final class Animation {
    let texture: StickerTexture
    /// Its own texture id; nil while it draws into an atlas, or after its
    /// atlas was disposed.
    var textureId: Int64?
    var atlas: StickerAtlas?
    init(texture: StickerTexture, textureId: Int64) {
      self.texture = texture
      self.textureId = textureId
    }
  }

  // Main thread only.
  private var animations: [Int64: Animation] = [:]
  private var lastAnimId: Int64 = 0
  private var atlases: [Int64: StickerAtlas] = [:]
  private var lastAtlasId: Int64 = 0
  /// Textures Dart may still draw for a frame or two after they were
  /// replaced, oldest first, until `releaseTextures`.
  private var retired: [(id: Int64, texture: RetiredTexture)] = []
  /// Retired textures kept at most; older ones are released even if Dart
  /// never asks.
  private static let maxRetired = 64
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
      wt_player_set_priority, wt_player_seek, wt_player_redraw, wt_atlas_options_init_default,
      wt_atlas_create, wt_atlas_destroy, wt_atlas_attach, wt_atlas_detach, wt_atlas_page_count,
      wt_atlas_get_page, wt_atlas_set_target, wt_atlas_shown_page,
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
    for (_, atlas) in atlases { _ = atlas.dispose(retire: retire) }
    atlases.removeAll()
    for animId in Array(animations.keys) { disposeNow(animId) }
    releaseRetired(retired.map(\.id))
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
      animations[animId] = Animation(texture: texture, textureId: textureId)
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

  func createAtlas(options: AtlasOptionsMessage) throws -> Int64 {
    guard (16...2048).contains(options.width), (16...2048).contains(options.maxHeight),
      (1...16).contains(options.maxPages)
    else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT", message: "atlas options out of range: \(options)",
        details: nil)
    }
    let atlas: WTAtlas
    do {
      atlas = try WTAtlas(
        width: UInt32(options.width), maxHeight: UInt32(options.maxHeight),
        maxPages: UInt32(options.maxPages))
    } catch let error as NSError {
      throw PigeonError(
        code: error.userInfo[WTStatusNameKey] as? String ?? "WT_ERR_?",
        message: "can't create the atlas", details: nil)
    }
    lastAtlasId += 1
    atlases[lastAtlasId] = StickerAtlas(atlasId: lastAtlasId, atlas: atlas)
    return lastAtlasId
  }

  func attachToAtlas(atlasId: Int64, animIds: [Int64]) throws -> AtlasLayoutMessage {
    guard let atlas = atlases[atlasId] else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT", message: "unknown atlas \(atlasId)", details: nil)
    }
    var retiredIds: [Int64] = []
    let slots = animIds.map { animId -> AtlasSlotMessage in
      var status = Int32(WT_ERR_INVALID_ARGUMENT)
      var slot = WTAtlasSlot()
      if let entry = animations[animId], entry.atlas == nil {
        status = atlas.atlas.attach(entry.texture.player, slot: &slot)
        if status == WT_OK {
          entry.atlas = atlas
          atlas.members.insert(animId)
          // Its frames go to the slot now; its own texture keeps the last
          // one until Dart has switched.
          if let id = entry.textureId {
            entry.textureId = nil
            entry.texture.detachTexture()
            retire(id, .standalone(entry.texture))
            retiredIds.append(id)
          }
        }
      }
      guard status == WT_OK else {
        return AtlasSlotMessage(
          animId: animId, pageId: -1, x: 0, y: 0, width: 0, height: 0,
          error: WTPlayer.name(forStatus: status))
      }
      return AtlasSlotMessage(
        animId: animId, pageId: Int64(slot.page), x: Int64(slot.x), y: Int64(slot.y),
        width: Int64(slot.width), height: Int64(slot.height))
    }
    retiredIds += atlas.sync(registry: registry, retire: retire)
    return AtlasLayoutMessage(
      slots: slots, handles: [], pages: atlas.pageMessages(), retiredTextureIds: retiredIds)
  }

  func detachFromAtlas(animIds: [Int64]) throws -> AtlasLayoutMessage {
    var touched: [StickerAtlas] = []
    let handles = animIds.map { animId -> AnimationHandleMessage in
      guard let entry = animations[animId] else {
        return AnimationHandleMessage(animId: animId, textureId: -1)
      }
      if let atlas = entry.atlas, atlas.atlas.detach(entry.texture.player) == WT_OK {
        atlas.members.remove(animId)
        entry.atlas = nil
        if !touched.contains(where: { $0 === atlas }) { touched.append(atlas) }
      }
      if entry.atlas == nil && entry.textureId == nil {
        // Back on a texture of its own, which gets the current frame.
        let textureId = registry.register(entry.texture)
        entry.texture.attach(textureId: textureId)
        entry.textureId = textureId
      }
      return AnimationHandleMessage(animId: animId, textureId: entry.textureId ?? -1)
    }
    var retiredIds: [Int64] = []
    var pages: [AtlasPageMessage] = []
    for atlas in touched {
      retiredIds += atlas.sync(registry: registry, retire: retire)
      pages += atlas.pageMessages()
    }
    return AtlasLayoutMessage(
      slots: [], handles: handles, pages: pages, retiredTextureIds: retiredIds)
  }

  func releaseTextures(textureIds: [Int64]) throws {
    releaseRetired(textureIds)
  }

  func atlasShownTextures(atlasId: Int64, animIds: [Int64]) throws -> [Int64] {
    guard let atlas = atlases[atlasId] else { return animIds.map { _ in -1 } }
    return animIds.map { animId in
      guard let entry = animations[animId], entry.atlas === atlas else { return -1 }
      return atlas.shownTexture(entry.texture.player)
    }
  }

  func atlasStats(atlasId: Int64) throws -> AtlasStatsMessage {
    guard let atlas = atlases[atlasId] else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT", message: "unknown atlas \(atlasId)", details: nil)
    }
    let own = animations.values.filter { $0.textureId != nil }.count
    let pages = atlases.values.reduce(0) { $0 + $1.textureCount }
    return AtlasStatsMessage(
      pages: atlas.stats(), textures: Int64(atlas.textureCount),
      pluginTextures: Int64(own + pages + retired.count))
  }

  func disposeAtlas(atlasId: Int64) throws -> AtlasLayoutMessage {
    guard let atlas = atlases.removeValue(forKey: atlasId) else {
      return AtlasLayoutMessage(slots: [], handles: [], pages: [], retiredTextureIds: [])
    }
    for animId in atlas.members { animations[animId]?.atlas = nil }
    return AtlasLayoutMessage(
      slots: [], handles: [], pages: [], retiredTextureIds: atlas.dispose(retire: retire))
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
    if let atlas = entry.atlas {
      // Leave the atlas here, on the main thread, so the player's close on a
      // background queue can't overlap the atlas being disposed.
      _ = atlas.atlas.detach(entry.texture.player)
      atlas.members.remove(animId)
    }
    // Unregister first so Flutter stops sampling, then free the decoder and
    // buffers once any in-flight frame finishes (off the main thread).
    if let id = entry.textureId { registry.unregisterTexture(id) }
    entry.texture.close()
  }

  /// Keeps `texture` (no longer drawn into) until Dart releases `id`.
  private func retire(_ id: Int64, _ texture: RetiredTexture) {
    retired.append((id, texture))
    while retired.count > Self.maxRetired {
      releaseRetired([retired[0].id])
    }
  }

  private func releaseRetired(_ ids: [Int64]) {
    for id in ids {
      guard let index = retired.firstIndex(where: { $0.id == id }) else { continue }
      let entry = retired.remove(at: index)
      registry.unregisterTexture(id)
      if case .standalone(let texture) = entry.texture,
        let owner = animations.values.first(where: { $0.texture === texture }),
        owner.textureId == nil
      {
        // Nothing shows its own buffers any more.
        texture.dropFrames()
      }
    }
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
