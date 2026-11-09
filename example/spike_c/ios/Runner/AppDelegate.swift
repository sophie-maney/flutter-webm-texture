import CoreVideo
import Darwin
import Flutter
import IOSurface
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

// Spike C (T0.9): N stickers at 30 fps as N FlutterTextures, or as one atlas
// FlutterTexture. Buffers are IOSurface BGRA CVPixelBuffers from a pool, as
// in spike B and ADR 0005. Kept in this file so the Xcode project needs no
// edits.

/// One FlutterTexture whose frames come from a CVPixelBufferPool.
final class PoolTexture: NSObject, FlutterTexture {
  let width: Int
  let height: Int
  var textureId: Int64 = 0
  private let lock = NSLock()
  private var current: CVPixelBuffer?  // guarded by lock
  private var pool: CVPixelBufferPool?
  private(set) var surfaces = Set<IOSurfaceID>()  // render queue only

  init(width: Int, height: Int) {
    self.width = width
    self.height = height
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

  // Raster thread. Hands Flutter a +1 reference to the latest buffer.
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }
    guard let b = current else { return nil }
    return Unmanaged.passRetained(b)
  }

  /// Fills a fresh pool buffer and publishes it. Render queue only.
  func produce(_ fill: (UnsafeMutablePointer<UInt8>, Int) -> Void) -> Bool {
    var out: CVPixelBuffer?
    guard let pool = pool,
      CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out) == kCVReturnSuccess,
      let buf = out
    else { return false }
    if let s = CVPixelBufferGetIOSurface(buf)?.takeUnretainedValue() {
      surfaces.insert(IOSurfaceGetID(s))
    }
    CVPixelBufferLockBaseAddress(buf, [])
    let base = CVPixelBufferGetBaseAddress(buf)!.assumingMemoryBound(to: UInt8.self)
    fill(base, CVPixelBufferGetBytesPerRow(buf))
    CVPixelBufferUnlockBaseAddress(buf, [])
    lock.lock()
    current = buf
    lock.unlock()
    return true
  }
}

final class Grid {
  private let registry: FlutterTextureRegistry
  private let atlas: Bool
  private let count: Int
  private let size: Int
  private let gutter: Int
  private let bank: Data
  private let frames: Int
  let textures: [PoolTexture]
  private let queue = DispatchQueue(label: "spike_c.render", qos: .userInteractive)
  private var timer: DispatchSourceTimer?
  private var tick = 0
  private let periodNs: UInt64
  private var nextNs: UInt64 = 0

  // Stats (render queue).
  private var workMs: [Double] = []
  private var lateMs: [Double] = []
  private var errors = 0
  private var notifies = 0

  init(registry: FlutterTextureRegistry, atlas: Bool, count: Int, size: Int, atlasW: Int,
       atlasH: Int, fps: Int, gutter: Int, bank: Data, frames: Int) {
    self.registry = registry
    self.atlas = atlas
    self.count = count
    self.size = size
    self.gutter = gutter
    self.bank = bank
    self.frames = frames
    self.periodNs = 1_000_000_000 / UInt64(fps)
    let n = atlas ? 1 : count
    textures = (0..<n).map { _ in
      atlas ? PoolTexture(width: atlasW, height: atlasH) : PoolTexture(width: size, height: size)
    }
    for t in textures { t.textureId = registry.register(t) }
    workMs.reserveCapacity(8192)
    lateMs.reserveCapacity(8192)
    let t = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
    t.schedule(deadline: .now(), repeating: .nanoseconds(Int(periodNs)), leeway: .nanoseconds(0))
    t.setEventHandler { [weak self] in self?.run() }
    nextNs = DispatchTime.now().uptimeNanoseconds
    t.resume()
    timer = t
  }

  private func run() {
    let start = DispatchTime.now().uptimeNanoseconds
    var failed = false
    bank.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
      let src = raw.bindMemory(to: UInt8.self).baseAddress!
      let frameBytes = size * size * 4
      func frame(_ i: Int) -> UnsafePointer<UInt8> {
        src + ((tick + i * 7) % frames) * frameBytes
      }
      if atlas {
        let pitch = size + 2 * gutter
        let tex = textures[0]
        let perRow = tex.width / pitch
        let ok = tex.produce { dst, stride in
          for i in 0..<count {
            let x = i % perRow * pitch, y = i / perRow * pitch
            if x + pitch > tex.width || y + pitch > tex.height { continue }
            blitCell(dst, stride, x, y, pitch, frame(i))
          }
        }
        if !ok { failed = true }
        registry.textureFrameAvailable(tex.textureId)
      } else {
        for (i, tex) in textures.enumerated() {
          let ok = tex.produce { dst, stride in blitCell(dst, stride, 0, 0, size, frame(i)) }
          if !ok { failed = true }
          registry.textureFrameAvailable(tex.textureId)
        }
      }
    }
    let end = DispatchTime.now().uptimeNanoseconds
    workMs.append(Double(end - start) / 1e6)
    lateMs.append((Double(start) - Double(nextNs)) / 1e6)
    if failed { errors += 1 }
    notifies += textures.count
    tick += 1
    nextNs += periodNs
    while nextNs < end { nextNs += periodNs }
  }

  /// Sticker at (x+gutter, y+gutter) in a `pitch` cell; gutter cleared.
  private func blitCell(_ dst: UnsafeMutablePointer<UInt8>, _ stride: Int, _ x: Int, _ y: Int,
                        _ pitch: Int, _ src: UnsafePointer<UInt8>) {
    let g = pitch == size ? 0 : gutter
    let rowBytes = size * 4
    for r in 0..<pitch {
      let row = dst + (y + r) * stride + x * 4
      let sr = r - g
      if sr < 0 || sr >= size {
        memset(row, 0, pitch * 4)
        continue
      }
      if g > 0 { memset(row, 0, g * 4) }
      memcpy(row + g * 4, src + sr * rowBytes, rowBytes)
      if g > 0 { memset(row + (g + size) * 4, 0, (pitch - g - size) * 4) }
    }
  }

  func resetStats() {
    queue.sync {
      workMs.removeAll(keepingCapacity: true)
      lateMs.removeAll(keepingCapacity: true)
      errors = 0
      notifies = 0
    }
  }

  func stats() -> [String: Any] {
    queue.sync {
      let surfaces = textures.map { $0.surfaces.count }
      return [
        "tickWorkMs": workMs,
        "tickLateMs": lateMs,
        "errors": errors,
        "notifies": notifies,
        "extra": [
          "distinctBuffers": surfaces.reduce(0, +),
          "maxBuffersPerWindow": surfaces.max() ?? 0,
          "windowsSeen": surfaces.count,
          "bufferBytes": (textures.first.map { $0.width * $0.height * 4 }) ?? 0,
          "producer": "CVPixelBufferPool BGRA IOSurface",
        ] as [String: Any],
      ]
    }
  }
}

final class SpikePlugin: NSObject {
  private let registry: FlutterTextureRegistry
  private var grid: Grid?

  init(registrar: FlutterPluginRegistrar) {
    registry = registrar.textures()
    super.init()
    let channel = FlutterMethodChannel(name: "spike_c", binaryMessenger: registrar.messenger())
    channel.setMethodCallHandler { [weak self] call, result in self?.handle(call, result) }
  }

  private func handle(_ call: FlutterMethodCall, _ result: @escaping FlutterResult) {
    let args = call.arguments as? [String: Any] ?? [:]
    switch call.method {
    case "info":
      let env = ProcessInfo.processInfo.environment
      result([
        "platform": "ios",
        "os": UIDevice.current.systemVersion,
        "device": env["SIMULATOR_DEVICE_NAME"].map { "\($0) (simulator)" } ?? UIDevice.current.model,
        "cores": ProcessInfo.processInfo.activeProcessorCount,
      ])
    case "start":
      let g = Grid(
        registry: registry,
        atlas: (args["mode"] as? String) == "atlas",
        count: args["count"] as! Int,
        size: args["size"] as! Int,
        atlasW: args["atlasW"] as! Int,
        atlasH: args["atlasH"] as! Int,
        fps: args["fps"] as! Int,
        gutter: args["gutter"] as! Int,
        bank: (args["bank"] as! FlutterStandardTypedData).data,
        frames: args["frames"] as! Int)
      grid = g
      NSLog("SpikeC started %d textures", g.textures.count)
      result([
        "textureIds": g.textures.map { $0.textureId },
        "producer": "CVPixelBufferPool BGRA IOSurface",
      ])
    case "resetStats":
      grid?.resetStats()
      result(nil)
    case "stats":
      result(grid?.stats() ?? [:])
    case "sample":
      result(Self.sample())
    default:
      result(FlutterMethodNotImplemented)
    }
  }

  /// Process CPU time and memory (phys_footprint includes IOSurfaces the
  /// process owns).
  static func sample() -> [String: Any] {
    var ru = rusage()
    getrusage(RUSAGE_SELF, &ru)
    let cpuMs =
      Double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000
      + Double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000
    var info = task_vm_info_data_t()
    var n = mach_msg_type_number_t(
      MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
    let kr = withUnsafeMutablePointer(to: &info) {
      $0.withMemoryRebound(to: integer_t.self, capacity: Int(n)) {
        task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &n)
      }
    }
    let ok = kr == KERN_SUCCESS
    return [
      "cpuMs": cpuMs,
      "rssKb": ok ? Int(info.resident_size / 1024) : -1,
      "footprintKb": ok ? Int(info.phys_footprint / 1024) : -1,
      "thermal": ProcessInfo.processInfo.thermalState.rawValue,
    ]
  }
}
