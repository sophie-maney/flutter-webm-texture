import CoreVideo
// Flutter's ObjC types predate Sendable; the registry is documented as
// usable from the worker that produced a frame (spike B).
@preconcurrency import Flutter
import Foundation
import wt_ios

/// One playing sticker: a decoder, a `FlutterTexture` and the worker that
/// feeds it.
///
/// Threads:
/// - `queue` (one serial queue per animation) owns the decoder and the
///   playback state. Every decode runs there, and `textureFrameAvailable` is
///   called from there; spike B found no need to hop to main.
/// - Flutter calls `copyPixelBuffer` on its raster thread; it only reads
///   `latest` under `lock`.
/// - `setActive` and `dispose` arrive on the main thread and post to `queue`.
///
/// Each frame is written into a fresh buffer from the pool, never into the
/// one Flutter may be sampling, and the newest one stays referenced so every
/// `copyPixelBuffer` call (including the extra one after foregrounding) gets
/// it back. T3.3 replaces the per-animation timer with the shared scheduler.
final class StickerTexture: NSObject, FlutterTexture {
  let animId: Int64
  let width: Int
  let height: Int

  /// Set on `queue` by `attach`, which the main thread posts right after
  /// registration and before playback can start.
  private var textureId: Int64?
  private weak var registry: FlutterTextureRegistry?
  private let onError: (Int64, String, String) -> Void

  private let queue: DispatchQueue
  private let pool: CVPixelBufferPool
  /// Caps buffers alive at once: the one Flutter holds, the one we hold as
  /// `latest`, one being written and one spare. When Flutter still holds
  /// more, the tick is skipped rather than growing the pool.
  private static let maxBuffers = 4
  private let poolAux =
    [kCVPixelBufferPoolAllocationThresholdKey as String: StickerTexture.maxBuffers]
    as CFDictionary

  private let lock = NSLock()
  private var latest: CVPixelBuffer?  // guarded by lock

  // Confined to `queue`.
  private var anim: WTAnimation?
  private var timer: DispatchSourceTimer?
  private var active = false
  private var nextDeadline = DispatchTime.now()
  private var lastDurationMs: UInt32 = StickerTexture.fallbackDurationMs
  private var reportedError = false

  /// Shortest frame we schedule (100 fps); also the fallback when a frame
  /// reports no duration.
  private static let minDurationMs: UInt32 = 10
  private static let fallbackDurationMs: UInt32 = 33

  /// Creates the decoder and renders frame 0, so the texture has content as
  /// soon as it is registered. Background thread: call it on `queue`.
  private init(
    animId: Int64, anim: WTAnimation, pool: CVPixelBufferPool, queue: DispatchQueue,
    registry: FlutterTextureRegistry, onError: @escaping (Int64, String, String) -> Void
  ) {
    self.animId = animId
    self.anim = anim
    self.width = Int(anim.width)
    self.height = Int(anim.height)
    self.pool = pool
    self.queue = queue
    self.registry = registry
    self.onError = onError
    super.init()
  }

  /// Builds an animation off the main thread and renders its first frame.
  /// Throws a `PigeonError` whose code is a `wt_status` name.
  static func create(
    animId: Int64, source: AnimationSourceMessage, bucketW: Int64, bucketH: Int64,
    registry: FlutterTextureRegistry, onError: @escaping (Int64, String, String) -> Void
  ) async throws -> StickerTexture {
    guard (source.bytes == nil) != (source.path == nil) else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT",
        message: "exactly one of bytes and path must be set", details: nil)
    }
    guard bucketW >= 0, bucketH >= 0, bucketW <= UInt32.max, bucketH <= UInt32.max else {
      throw PigeonError(
        code: "WT_ERR_INVALID_ARGUMENT", message: "bucket \(bucketW)x\(bucketH)", details: nil)
    }
    let queue = DispatchQueue(label: "webm_texture.anim.\(animId)", qos: .userInitiated)
    return try await withCheckedThrowingContinuation { cont in
      queue.async {
        do {
          let w = UInt32(bucketW)
          let h = UInt32(bucketH)
          let anim: WTAnimation
          if let bytes = source.bytes {
            anim = try WTAnimation(data: bytes.data, width: w, height: h)
          } else {
            anim = try WTAnimation(path: source.path!, width: w, height: h)
          }
          guard let pool = makePool(width: Int(anim.width), height: Int(anim.height)) else {
            anim.close()
            throw PigeonError(
              code: "WT_ERR_NO_MEMORY", message: "CVPixelBufferPoolCreate failed", details: nil)
          }
          let texture = StickerTexture(
            animId: animId, anim: anim, pool: pool, queue: queue, registry: registry,
            onError: onError)
          if let status = texture.renderFrame(), status < 0 {
            // No first frame means nothing to show: fail the create.
            anim.close()
            throw PigeonError(
              code: WTAnimation.name(forStatus: status), message: "first frame failed",
              details: nil)
          }
          cont.resume(returning: texture)
        } catch let error as NSError where error.domain == WTStatusErrorDomain {
          cont.resume(
            throwing: PigeonError(
              code: error.userInfo[WTStatusNameKey] as? String ?? "WT_ERR_?",
              message: "the core rejected the sticker", details: nil))
        } catch {
          cont.resume(throwing: error)
        }
      }
    }
  }

  private static func makePool(width: Int, height: Int) -> CVPixelBufferPool? {
    let attrs: [String: Any] = [
      kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
      kCVPixelBufferWidthKey as String: width,
      kCVPixelBufferHeightKey as String: height,
      kCVPixelBufferIOSurfacePropertiesKey as String: [String: Any](),
      kCVPixelBufferMetalCompatibilityKey as String: true,
    ]
    var pool: CVPixelBufferPool?
    guard CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool) == kCVReturnSuccess
    else { return nil }
    return pool
  }

  // MARK: FlutterTexture

  /// Raster thread. Returns a +1 reference to the newest complete frame.
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }
    guard let buffer = latest else { return nil }
    return Unmanaged.passRetained(buffer)
  }

  // MARK: Main-thread entry points

  /// Main thread, once, right after `registry.register(self)`. Frame 0 was
  /// rendered before registration, so announce it.
  func attach(textureId: Int64) {
    queue.async { [self] in
      self.textureId = textureId
      registry?.textureFrameAvailable(textureId)
    }
  }

  /// Starts or pauses playback. A paused animation keeps showing its
  /// current frame.
  func setActive(_ on: Bool) {
    queue.async { [self] in
      guard anim != nil, on != active else { return }
      active = on
      if on {
        startTimer()
      } else {
        timer?.cancel()
        timer = nil
      }
    }
  }

  /// Stops playback and releases the decoder and every buffer. The caller
  /// unregisters the texture first, so Flutter stops asking for frames.
  func close() {
    queue.async { [self] in
      timer?.cancel()
      timer = nil
      active = false
      anim?.close()
      anim = nil
      lock.lock()
      latest = nil
      lock.unlock()
      CVPixelBufferPoolFlush(pool, .excessBuffers)
    }
  }

  // MARK: Playback (on `queue`)

  private func startTimer() {
    let t = DispatchSource.makeTimerSource(queue: queue)
    t.setEventHandler { [weak self] in self?.tick() }
    // The frame on screen was shown for an unknown time; give it one
    // duration from now.
    nextDeadline = .now() + .milliseconds(Int(lastDurationMs))
    t.schedule(deadline: nextDeadline, leeway: .milliseconds(2))
    t.resume()
    timer = t
  }

  private func tick() {
    guard active, let timer = timer else { return }
    if renderFrame() == nil {
      // The pool is at its cap: Flutter hasn't released old buffers yet.
      // Try again shortly without moving the cursor.
      timer.schedule(deadline: .now() + .milliseconds(Int(Self.minDurationMs)), leeway: .milliseconds(2))
      return
    }
    let now = DispatchTime.now()
    nextDeadline = nextDeadline + .milliseconds(Int(lastDurationMs))
    // More than a frame behind (a stall, or the app was suspended): restart
    // the clock from now instead of rushing through frames to catch up.
    if nextDeadline.uptimeNanoseconds + UInt64(lastDurationMs) * 1_000_000 < now.uptimeNanoseconds {
      nextDeadline = now + .milliseconds(Int(lastDurationMs))
    }
    timer.schedule(deadline: nextDeadline, leeway: .milliseconds(2))
  }

  /// Decodes the next frame into a fresh pool buffer and publishes it.
  /// Returns the wt_status, or nil when no buffer was available (nothing
  /// decoded, cursor unchanged).
  @discardableResult
  private func renderFrame() -> Int32? {
    guard let anim = anim else { return nil }
    var out: CVPixelBuffer?
    let rv = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(nil, pool, poolAux, &out)
    guard rv == kCVReturnSuccess, let buffer = out else { return nil }

    var durationMs: UInt32 = 0
    let status = anim.renderNextFrame(into: buffer, durationMs: &durationMs)
    if durationMs > 0 { lastDurationMs = max(durationMs, Self.minDurationMs) }
    guard status == 0 else {
      // The cursor moved on; keep showing the previous frame. Report the
      // first failure only, so a broken file doesn't flood the channel.
      if !reportedError {
        reportedError = true
        onError(
          animId, WTAnimation.name(forStatus: status), "frame decode failed mid-playback")
      }
      return status
    }
    lock.lock()
    latest = buffer
    lock.unlock()
    if let id = textureId { registry?.textureFrameAvailable(id) }
    return status
  }
}
