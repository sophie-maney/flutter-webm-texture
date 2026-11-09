import CoreVideo
import Flutter
import UIKit

@main
@objc class AppDelegate: FlutterAppDelegate, FlutterImplicitEngineDelegate {
  private var spike: SpikePlugin?

  override func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
  ) -> Bool {
    return super.application(application, didFinishLaunchingWithOptions: launchOptions)
  }

  func didInitializeImplicitFlutterEngine(_ engineBridge: FlutterImplicitEngineBridge) {
    GeneratedPluginRegistrant.register(with: engineBridge.pluginRegistry)
    if let registrar = engineBridge.pluginRegistry.registrar(forPlugin: "SpikePlugin") {
      spike = SpikePlugin(registrar: registrar)
    }
  }
}

// Spike B (T0.8): a FlutterTexture backed by IOSurface BGRA CVPixelBuffers,
// filled from CPU memory as the real plugin will (PLAN.md, T2.3). Kept in this
// file so the Xcode project needs no edits.

final class SpikeTexture: NSObject, FlutterTexture {
  let kind: String
  let width: Int
  let height: Int
  private let rgba: Data?
  private let notifyOnMain: Bool
  private weak var registry: FlutterTextureRegistry?
  var textureId: Int64 = 0

  private let queue: DispatchQueue
  private let lock = NSLock()
  private var current: CVPixelBuffer?  // guarded by lock
  private var pool: CVPixelBufferPool?
  private var timer: DispatchSourceTimer?
  private var frame = 0

  // Counters (guarded by lock).
  private var draws = 0
  private var copies = 0
  private var errors = 0
  private var copiesOffMain = 0

  init(kind: String, width: Int, height: Int, rgba: Data?, notifyOnMain: Bool,
       registry: FlutterTextureRegistry) {
    self.kind = kind
    self.width = width
    self.height = height
    self.rgba = rgba
    self.notifyOnMain = notifyOnMain
    self.registry = registry
    self.queue = DispatchQueue(label: "spike_b.\(kind)")
    super.init()
    let attrs: [String: Any] = [
      kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
      kCVPixelBufferWidthKey as String: width,
      kCVPixelBufferHeightKey as String: height,
      kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
      kCVPixelBufferMetalCompatibilityKey as String: true,
    ]
    CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
  }

  func start() {
    if kind == "static" {
      queue.async { self.drawStatic() }
    } else {
      let t = DispatchSource.makeTimerSource(queue: queue)
      t.schedule(deadline: .now(), repeating: .milliseconds(33))
      t.setEventHandler { [weak self] in self?.drawAnim() }
      t.resume()
      timer = t
    }
  }

  func redraw() {
    if kind == "static" { queue.async { self.drawStatic() } }
  }

  // Raster thread. Hands Flutter a +1 reference to the latest buffer.
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }
    copies += 1
    if !Thread.isMainThread { copiesOffMain += 1 }
    guard let b = current else { return nil }
    return Unmanaged.passRetained(b)
  }

  func stats() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }
    return ["draws": draws, "errors": errors, "copies": copies, "copiesOffMain": copiesOffMain]
  }

  /// Gets a fresh buffer from the pool (never the one Flutter may be
  /// sampling), fills it, publishes it and notifies Flutter.
  private func produce(_ fill: (UnsafeMutablePointer<UInt8>, Int) -> Void) {
    var out: CVPixelBuffer?
    guard let pool = pool,
          CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess,
          let buf = out
    else {
      lock.lock(); errors += 1; lock.unlock()
      return
    }
    CVPixelBufferLockBaseAddress(buf, [])
    let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
    fill(base, CVPixelBufferGetBytesPerRow(buf))
    CVPixelBufferUnlockBaseAddress(buf, [])
    lock.lock()
    current = buf
    draws += 1
    lock.unlock()
    let id = textureId
    if notifyOnMain {
      DispatchQueue.main.async { [weak self] in self?.registry?.textureFrameAvailable(id) }
    } else {
      registry?.textureFrameAvailable(id)
    }
  }

  /// RGBA (premultiplied) -> BGRA (premultiplied): swap R and B.
  private func drawStatic() {
    guard let rgba = rgba else { return }
    let w = width, h = height
    rgba.withUnsafeBytes { (src: UnsafeRawBufferPointer) in
      let s = src.bindMemory(to: UInt8.self)
      produce { dst, stride in
        for y in 0..<h {
          let row = dst + y * stride
          for x in 0..<w {
            let i = (y * w + x) * 4
            row[x * 4 + 0] = s[i + 2]
            row[x * 4 + 1] = s[i + 1]
            row[x * 4 + 2] = s[i + 0]
            row[x * 4 + 3] = s[i + 3]
          }
        }
      }
    }
  }

  /// Same animated pattern as Android (spike_b.cc drawAnim), in BGRA.
  private func drawAnim() {
    let f = frame
    frame += 1
    let a = UInt8((f * 4) & 0xff)
    let w = width, h = height, bw = width / 8
    produce { dst, stride in
      for y in 0..<h {
        let row = dst + y * stride
        for x in 0..<w {
          let p = row + x * 4
          if y < 16 {
            let v: UInt8 = ((f >> (7 - x / bw)) & 1) == 1 ? 255 : 0
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255
          } else {
            p[0] = 0; p[1] = 0; p[2] = a; p[3] = a
          }
        }
      }
    }
  }
}

final class SpikePlugin: NSObject {
  private let registry: FlutterTextureRegistry
  private var textures: [SpikeTexture] = []

  init(registrar: FlutterPluginRegistrar) {
    registry = registrar.textures()
    super.init()
    let channel = FlutterMethodChannel(name: "spike_b", binaryMessenger: registrar.messenger())
    channel.setMethodCallHandler { [weak self] call, result in self?.handle(call, result) }
  }

  private func handle(_ call: FlutterMethodCall, _ result: FlutterResult) {
    let args = call.arguments as? [String: Any] ?? [:]
    switch call.method {
    case "info":
      let env = ProcessInfo.processInfo.environment
      result([
        "platform": "ios",
        "os": UIDevice.current.systemVersion,
        "device": env["SIMULATOR_DEVICE_NAME"].map { "\($0) (simulator)" } ?? UIDevice.current.model,
        "wideGamut": "\(Bundle.main.object(forInfoDictionaryKey: "FLTEnableWideGamut") ?? "default")",
      ])
    case "create":
      let t = SpikeTexture(
        kind: args["kind"] as! String,
        width: args["width"] as! Int,
        height: args["height"] as! Int,
        rgba: (args["rgba"] as? FlutterStandardTypedData)?.data,
        notifyOnMain: (args["notify"] as? String) == "main",
        registry: registry)
      t.textureId = registry.register(t)
      textures.append(t)
      t.start()
      NSLog("SpikeB created %@ id=%lld", t.kind, t.textureId)
      result(["textureId": t.textureId, "path": "CVPixelBuffer BGRA IOSurface"])
    case "redraw":
      textures.forEach { $0.redraw() }
      result(nil)
    case "stats":
      var out: [String: Any] = [:]
      for t in textures { out[t.kind] = t.stats() }
      result(out)
    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
