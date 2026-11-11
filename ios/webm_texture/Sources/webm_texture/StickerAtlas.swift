import CoreVideo
// Flutter's ObjC types predate Sendable; the registry is documented as
// usable from the worker that produced a frame (spike B).
@preconcurrency import Flutter
import Foundation
import wt_core
import wt_ios

/// One atlas page's `FlutterTexture` (T4.2): the newest complete page that
/// the core presented into its `WTAtlasPage`.
///
/// Threads: the scheduler's workers present into the page and call
/// `frameReady`, which calls `textureFrameAvailable` from there, once per page
/// per tick. Flutter calls `copyPixelBuffer` on its raster thread. `attach`
/// runs on the main thread.
final class AtlasPageTexture: NSObject, FlutterTexture {
  let page: WTAtlasPage
  private weak var registry: FlutterTextureRegistry?

  private let lock = NSLock()
  private var textureId: Int64?  // guarded by lock
  private var hasFrame = false  // guarded by lock

  private init(page: WTAtlasPage, registry: FlutterTextureRegistry) {
    self.page = page
    self.registry = registry
    super.init()
  }

  /// A page texture of `width` x `height`, or nil when its pool can't be
  /// created.
  static func make(width: UInt32, height: UInt32, registry: FlutterTextureRegistry)
    -> AtlasPageTexture?
  {
    // The handler runs on scheduler workers and must not keep the texture
    // alive.
    let box = WeakPageTexture()
    guard let page = WTAtlasPage(width: width, height: height, onFrame: { box.value?.frameReady() })
    else { return nil }
    let texture = AtlasPageTexture(page: page, registry: registry)
    box.value = texture
    return texture
  }

  /// Raster thread. Returns a +1 reference to the newest page.
  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    guard let buffer = page.copyLatestFrame() else { return nil }
    return Unmanaged.passRetained(buffer)
  }

  /// Main thread, right after `registry.register(self)`.
  func attach(textureId: Int64) {
    lock.lock()
    self.textureId = textureId
    let ready = hasFrame
    lock.unlock()
    if ready { registry?.textureFrameAvailable(textureId) }
  }

  fileprivate func frameReady() {
    lock.lock()
    hasFrame = true
    let id = textureId
    lock.unlock()
    if let id { registry?.textureFrameAvailable(id) }
  }
}

private final class WeakPageTexture: @unchecked Sendable {
  weak var value: AtlasPageTexture?
}

/// A texture Dart may still be drawing after it was replaced, kept until
/// `releaseTextures`.
enum RetiredTexture {
  case page(AtlasPageTexture)
  case standalone(StickerTexture)
}

/// One `WTAtlas` and the texture each of its pages draws into. Main thread
/// only.
///
/// A page's buffers have a fixed size, so when the core wants a page taller
/// or shorter (`wantedHeight`), `sync` registers a new texture of that
/// height and retires the old one: it keeps its last frame until Dart, which
/// lays slots out at the old size until it gets the new id, releases it.
final class StickerAtlas {
  let atlasId: Int64
  let atlas: WTAtlas
  /// Animations in the atlas.
  var members = Set<Int64>()

  private struct PageSurface {
    let texture: AtlasPageTexture
    let textureId: Int64
    let width: Int
    let height: Int
  }
  private var pages: [Int: PageSurface] = [:]

  init(atlasId: Int64, atlas: WTAtlas) {
    self.atlasId = atlasId
    self.atlas = atlas
  }

  var textureCount: Int { pages.count }

  /// Gives every page a texture of the height it wants now, or none.
  /// Returns the ids it retired.
  func sync(registry: FlutterTextureRegistry, retire: (Int64, RetiredTexture) -> Void) -> [Int64] {
    var retired: [Int64] = []
    for page in 0..<atlas.pageCount {
      var info = WTAtlasPageInfo()
      guard atlas.getPage(page, info: &info) == WT_OK else { continue }
      let wanted = Int(info.wantedHeight)
      let current = pages[Int(page)]
      if (current?.height ?? 0) == wanted { continue }
      var next: PageSurface?
      if wanted > 0 {
        guard
          let texture = AtlasPageTexture.make(
            width: info.width, height: UInt32(wanted), registry: registry)
        else { continue }
        let textureId = registry.register(texture)
        texture.attach(textureId: textureId)
        guard atlas.setPage(page, target: texture.page, height: UInt32(wanted)) == WT_OK else {
          registry.unregisterTexture(textureId)
          continue
        }
        next = PageSurface(
          texture: texture, textureId: textureId, width: Int(info.width), height: wanted)
      } else {
        _ = atlas.setPage(page, target: nil, height: 0)
      }
      if let current {
        retire(current.textureId, .page(current.texture))
        retired.append(current.textureId)
      }
      pages[Int(page)] = next
    }
    return retired
  }

  /// Every page that has a texture.
  func pageMessages() -> [AtlasPageMessage] {
    pages.keys.sorted().compactMap { page in
      guard let p = pages[page] else { return nil }
      return AtlasPageMessage(
        atlasId: atlasId, pageId: Int64(page), textureId: p.textureId, width: Int64(p.width),
        height: Int64(p.height))
    }
  }

  func stats() -> [AtlasPageStatsMessage] {
    (0..<atlas.pageCount).compactMap { page in
      var info = WTAtlasPageInfo()
      guard atlas.getPage(page, info: &info) == WT_OK else { return nil }
      let surface = pages[Int(page)]
      return AtlasPageStatsMessage(
        pageId: Int64(page), textureId: surface?.textureId ?? -1, width: Int64(info.width),
        height: Int64(surface?.height ?? 0), usedHeight: Int64(info.usedHeight),
        liveSlots: Int64(info.liveSlots), buffers: Int64(info.buffers),
        bufferBytes: Int64(info.bufferBytes), cpuBytes: Int64(info.cpuBytes),
        presents: Int64(info.presents), presentFailures: Int64(info.presentFailures))
    }
  }

  /// Frees the atlas (its animations go back to their own, retired,
  /// buffers) and retires every page texture. Returns their ids.
  func dispose(retire: (Int64, RetiredTexture) -> Void) -> [Int64] {
    // Waits for presents in flight; no page is written afterwards.
    atlas.close()
    var retired: [Int64] = []
    for (_, p) in pages {
      retire(p.textureId, .page(p.texture))
      retired.append(p.textureId)
    }
    pages.removeAll()
    members.removeAll()
    return retired
  }
}
