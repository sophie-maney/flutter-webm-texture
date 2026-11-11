// WTAtlas and WTAtlasPage: the ObjC++ shim between the Swift atlas glue and
// wt_atlas (T4.2, ADR 0020).
//
// A WTAtlas owns one wt_atlas on the process-wide scheduler. Each page that
// has slots gets a WTAtlasPage from the Swift side: a BGRA IOSurface pool of
// the page's size that the core presents the whole page into, once per
// scheduler tick, keeping the newest complete page for -copyLatestFrame.
// Flutter is told with one textureFrameAvailable per page per tick (the
// onFrame handler).
//
// Threads:
// - WTAtlas's methods run on the main thread. -attachPlayer:, -detachPlayer:
//   and -setPage:target:height: wait for at most one frame or present in
//   flight; -close waits for all of them.
// - WTAtlasPage's onFrame handler runs on scheduler workers; it must not
//   keep the page's texture alive (capture it weakly). -copyLatestFrame may
//   be called from any thread (Flutter's raster thread).
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#import "WTPlayer.h"

NS_ASSUME_NONNULL_BEGIN

/// Where an attached player is drawn: its content rect in page pixels
/// (wt_atlas_slot).
typedef struct WTAtlasSlot {
  uint32_t page;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
} WTAtlasSlot;

/// wt_atlas_page_info.
typedef struct WTAtlasPageInfo {
  uint32_t width;
  uint32_t usedHeight;
  uint32_t wantedHeight;
  uint32_t height;
  uint32_t hasTarget;
  uint32_t liveSlots;
  uint32_t buffers;
  uint32_t bufferStride;
  uint64_t bufferBytes;
  uint64_t cpuBytes;
  uint64_t presents;
  uint64_t presentFailures;
} WTAtlasPageInfo;

/// Called on a scheduler worker each time a new page frame is ready.
typedef void (^WTPageFrameHandler)(void);

NS_SWIFT_NAME(WTAtlasPage)
@interface WTAtlasPage : NSObject

/// A pool of width x height BGRA IOSurface buffers. Returns nil when the
/// pool can't be created.
- (nullable instancetype)initWithWidth:(uint32_t)width
                                height:(uint32_t)height
                               onFrame:(WTPageFrameHandler)onFrame;

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

@property(nonatomic, readonly) uint32_t width;
@property(nonatomic, readonly) uint32_t height;

/// The newest complete page, retained for the caller, or NULL before the
/// first. Any thread.
- (nullable CVPixelBufferRef)copyLatestFrame CF_RETURNS_RETAINED;

@end

NS_SWIFT_NAME(WTAtlas)
@interface WTAtlas : NSObject

/// An empty atlas on the process-wide scheduler: pages `width` wide (rounded
/// down to 16), up to `maxHeight` tall, at most `maxPages` of them, in BGRA.
/// Fails with a WTStatusErrorDomain error for options out of range.
- (nullable instancetype)initWithWidth:(uint32_t)width
                             maxHeight:(uint32_t)maxHeight
                              maxPages:(uint32_t)maxPages
                                 error:(NSError **)error;

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

/// Moves `player` into a slot of its size (wt_atlas_attach). Returns a
/// wt_status; on WT_OK *slot says where.
- (int32_t)attachPlayer:(WTPlayer *)player slot:(WTAtlasSlot *)slot;

/// Moves `player` back to its own buffers (wt_atlas_detach). Returns a
/// wt_status.
- (int32_t)detachPlayer:(WTPlayer *)player;

/// The page whose current target shows `player`'s slot, or -1
/// (wt_atlas_shown_page). Never waits for a frame.
- (int32_t)shownPageForPlayer:(WTPlayer *)player;

/// Pages created so far (0 after -close).
@property(nonatomic, readonly) uint32_t pageCount;

/// Fills *info for `page`. Returns a wt_status.
- (int32_t)getPage:(uint32_t)page info:(WTAtlasPageInfo *)info;

/// Points `page` at `target`, of the atlas's width x `height`; nil leaves it
/// without one for now, keeping `height` (0: for good). Waits for a present
/// in flight on the old target, which is never written again. Returns a
/// wt_status.
- (int32_t)setPage:(uint32_t)page
            target:(nullable WTAtlasPage *)target
            height:(uint32_t)height;

/// Gives every player back its own sink and frees the atlas, waiting for
/// presents in flight (wt_atlas_destroy). Idempotent.
- (void)close;

@end

NS_ASSUME_NONNULL_END
