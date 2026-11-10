import Flutter
import UIKit

import wt_core

public class WebmTexturePlugin: NSObject, FlutterPlugin, WebmTextureHostApi {
  /// Dart finds the core with DynamicLibrary.process(). The app is statically
  /// linked, so reference every entry point from Swift, or the linker would
  /// strip the ones nothing else uses.
  private static var keepAlive: [Any] = []

  public static func register(with registrar: FlutterPluginRegistrar) {
    keepAlive = [
      wt_api_version, wt_status_string, wt_limits_init_default,
      wt_source_create_memory, wt_source_create_file, wt_source_destroy,
      wt_probe, wt_anim_create, wt_anim_destroy, wt_anim_get_info, wt_anim_seek,
      wt_anim_next_frame,
    ]
    let channel = FlutterMethodChannel(name: "webm_texture", binaryMessenger: registrar.messenger())
    let instance = WebmTexturePlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
    WebmTextureHostApiSetup.setUp(binaryMessenger: registrar.messenger(), api: instance)
  }

  // T2.3 replaces these stubs with the FlutterTexture glue.
  func createAnimation(
    sourceKey: String, source: AnimationSourceMessage, bucketW: Int64, bucketH: Int64
  ) async throws -> AnimationHandleMessage {
    throw Self.notImplemented("createAnimation")
  }

  func setActive(animId: Int64, active: Bool) throws {
    throw Self.notImplemented("setActive")
  }

  func dispose(animId: Int64) throws {
    throw Self.notImplemented("dispose")
  }

  // T3.4 reports the real device signals.
  func deviceSignals() throws -> DeviceSignalsMessage {
    throw Self.notImplemented("deviceSignals")
  }

  private static func notImplemented(_ method: String) -> PigeonError {
    PigeonError(code: "UNIMPLEMENTED", message: "\(method) is not implemented on iOS yet", details: nil)
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
