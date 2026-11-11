import CoreVideo
// Flutter's ObjC types predate Sendable; the registry is documented as
// usable from the worker that produced a frame (spike B).
@preconcurrency import Flutter
import Foundation
import wt_ios

/// One playing sticker: a `FlutterTexture` over a `WTPlayer`, which the
/// core's process-wide scheduler (T3.3) feeds. While the animation draws
/// into an atlas (T4.2) it has no texture id of its own (`detachTexture`);
/// leaving the atlas registers it again under a new id.
///
/// Threads:
/// - The scheduler's workers decode into fresh pool buffers and call
///   `frameReady`, which calls `textureFrameAvailable` from there; spike B
///   found no need to hop to main.
/// - Flutter calls `copyPixelBuffer` on its raster thread; it returns the
///   player's newest complete frame, also after foregrounding.
/// - `attach`, `setActive`, `setPriority`, `seek` and `close` arrive on the
///   main thread and never decode; `close` hands the wait for a frame in
///   flight to a background queue.
final class StickerTexture: NSObject, FlutterTexture {
  let animId: Int64
  let width: Int
  let height: Int
  let frameCount: Int

  /// The shim, for `WTAtlas`; everything else goes through this class.
  let player: WTPlayer
  private weak var registry: FlutterTextureRegistry?

  private let lock = NSLock()
  private var textureId: Int64?  // guarded by lock
  private var closed = false  // guarded by lock

  private init(animId: Int64, player: WTPlayer, registry: FlutterTextureRegistry) {
    self.animId = animId
    self.player = player
    self.width = Int(player.width)
    self.height = Int(player.height)
    self.frameCount = Int(player.frameCount)
    self.registry = registry
    super.init()
  }

  /// Builds an animation off the main thread and puts it on the scheduler,
  /// which draws its first frame right away. With `loop` false, playback
  /// stops on the last frame. Frames are cached in `cacheDirectory` under a
  /// name made from `sourceKey` and the bucket. Throws a `PigeonError` whose
  /// code is a `wt_status` name.
  static func create(
    animId: Int64, source: AnimationSourceMessage, sourceKey: String, bucketW: Int64,
    bucketH: Int64, loop: Bool, cacheDirectory: String, registry: FlutterTextureRegistry,
    onError: @escaping @Sendable (Int64, String, String) -> Void
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
    return try await withCheckedThrowingContinuation { cont in
      DispatchQueue.global(qos: .userInitiated).async {
        do {
          let w = UInt32(bucketW)
          let h = UInt32(bucketH)
          // The core writes cache files here but never creates the directory.
          try? FileManager.default.createDirectory(
            atPath: cacheDirectory, withIntermediateDirectories: true)
          // The handlers run on scheduler workers and must not keep the
          // player (or the texture that owns it) alive.
          let box = WeakTexture()
          let onFrame: WTFrameHandler = { box.value?.frameReady() }
          let onDecodeError: WTErrorHandler = { status in
            onError(animId, WTPlayer.name(forStatus: status), "frame decode failed mid-playback")
          }
          let player: WTPlayer
          if let bytes = source.bytes {
            player = try WTPlayer(
              data: bytes.data, width: w, height: h, loop: loop,
              cacheDirectory: cacheDirectory, sourceKey: sourceKey, onFrame: onFrame,
              onError: onDecodeError)
          } else {
            player = try WTPlayer(
              path: source.path!, width: w, height: h, loop: loop,
              cacheDirectory: cacheDirectory, sourceKey: sourceKey, onFrame: onFrame,
              onError: onDecodeError)
          }
          let texture = StickerTexture(animId: animId, player: player, registry: registry)
          box.value = texture
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

  /// A texture released without `close` (the engine went away without
  /// detaching) still frees its player, off the current thread: this may be
  /// a scheduler worker inside the player's own frame handler, where
  /// `close` would wait for itself.
  deinit {
    if !closed {
      DispatchQueue.global(qos: .userInitiated).async { [player] in player.close() }
    }
  }

  // MARK: FlutterTexture

  /// Raster thread. Returns a +1 reference to the newest complete frame.
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    guard let buffer = player.copyLatestFrame() else { return nil }
    return Unmanaged.passRetained(buffer)
  }

  // MARK: Main-thread entry points

  /// Main thread, right after `registry.register(self)`: at creation, and
  /// again when the animation leaves an atlas. A frame that was ready before
  /// the id was known is announced here.
  func attach(textureId: Int64) {
    lock.lock()
    self.textureId = textureId
    lock.unlock()
    // Draws the current frame if the player waited without a target.
    player.setHasTarget(true)
    // Set the id first: a frame landing now is announced by frameReady,
    // one that landed before is seen here.
    if player.hasFrame { registry?.textureFrameAvailable(textureId) }
  }

  /// Main thread: the animation now draws into an atlas. The old id stays
  /// registered (showing the last frame) until the plugin releases it.
  func detachTexture() {
    lock.lock()
    textureId = nil
    lock.unlock()
    player.setHasTarget(false)
  }

  /// Frees the player's own buffers once no texture id shows them.
  func dropFrames() {
    player.dropFrames()
  }

  /// Starts or pauses playback. A paused animation keeps showing its
  /// current frame, and so does one that ended.
  func setActive(_ on: Bool) {
    player.setActive(on)
  }

  /// The on-screen area of the widgets playing this animation, in physical
  /// pixels: its rank under the scheduler's `maxConcurrent` limit.
  func setPriority(_ priority: Int64) {
    player.setPriority(UInt64(max(priority, 0)))
  }

  /// Shows `frame` as soon as a worker is free, active or not; an active
  /// animation carries on from there. The caller checks `frame` against
  /// `frameCount`.
  func seek(toFrame frame: Int) {
    guard frame >= 0, frame < frameCount else { return }
    _ = player.seek(toFrame: UInt32(frame))
  }

  /// Stops playback and releases the decoder and every buffer. The caller
  /// unregisters the texture first, so Flutter stops asking for frames.
  func close() {
    lock.lock()
    closed = true
    lock.unlock()
    // Waits for a frame in flight, so keep it off the main thread.
    DispatchQueue.global(qos: .userInitiated).async { [player] in player.close() }
  }

  // MARK: Scheduler workers

  /// A new frame is ready: tell Flutter, unless the texture is gone.
  fileprivate func frameReady() {
    lock.lock()
    let id = closed ? nil : textureId
    lock.unlock()
    if let id { registry?.textureFrameAvailable(id) }
  }
}

/// A weak reference to a texture that the player's handlers can carry.
private final class WeakTexture: @unchecked Sendable {
  weak var value: StickerTexture?
}
