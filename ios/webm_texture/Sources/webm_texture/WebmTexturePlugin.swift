import Flutter
import UIKit

import wt_core

public class WebmTexturePlugin: NSObject, FlutterPlugin, WebmTextureHostApi {
  /// Dart finds the core with DynamicLibrary.process(). The app is statically
  /// linked, so reference every entry point from Swift, or the linker would
  /// strip the ones nothing else uses.
  private static var keepAlive: [Any] = []

  private let registry: FlutterTextureRegistry
  private let errors = ErrorStream()

  // Main thread only.
  private var animations: [Int64: (texture: StickerTexture, textureId: Int64)] = [:]
  private var lastAnimId: Int64 = 0
  private var detached = false

  init(registry: FlutterTextureRegistry) {
    self.registry = registry
    super.init()
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    keepAlive = [
      wt_api_version, wt_status_string, wt_limits_init_default,
      wt_source_create_memory, wt_source_create_file, wt_source_destroy,
      wt_probe, wt_anim_create, wt_anim_destroy, wt_anim_get_info, wt_anim_seek,
      wt_anim_next_frame,
    ]
    let channel = FlutterMethodChannel(name: "webm_texture", binaryMessenger: registrar.messenger())
    let instance = WebmTexturePlugin(registry: registrar.textures())
    registrar.addMethodCallDelegate(instance, channel: channel)
    // Publishing makes the engine call detachFromEngine(for:) on teardown.
    registrar.publish(instance)
    WebmTextureHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: instance)
    StreamErrorsStreamHandler.register(with: registrar.messenger(), streamHandler: instance.errors)
  }

  public func detachFromEngine(for registrar: FlutterPluginRegistrar) {
    detached = true
    for animId in Array(animations.keys) { disposeNow(animId) }
    WebmTextureHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: nil)
  }

  // MARK: WebmTextureHostApi

  func createAnimation(
    sourceKey: String, source: AnimationSourceMessage, bucketW: Int64, bucketH: Int64
  ) async throws -> AnimationHandleMessage {
    let animId = await MainActor.run { () -> Int64 in
      lastAnimId += 1
      return lastAnimId
    }
    // Decodes frame 0 on the animation's own queue, off the main thread.
    let texture = try await StickerTexture.create(
      animId: animId, source: source, bucketW: bucketW, bucketH: bucketH, registry: registry,
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

  func dispose(animId: Int64) throws {
    disposeNow(animId)
  }

  // T3.4 reports the real device signals.
  func deviceSignals() throws -> DeviceSignalsMessage {
    throw PigeonError(
      code: "UNIMPLEMENTED", message: "deviceSignals is not implemented on iOS yet", details: nil)
  }

  // MARK: Internals (main thread)

  private func disposeNow(_ animId: Int64) {
    guard let entry = animations.removeValue(forKey: animId) else { return }
    // Unregister first so Flutter stops sampling, then free the decoder and
    // buffers on the animation's queue once any in-flight frame finishes.
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
