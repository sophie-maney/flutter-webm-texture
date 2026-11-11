// WTPlayer: the ObjC++ shim between the Swift texture glue and wt_api.h.
//
// It owns one animation on the process-wide scheduler (wt_sched, T3.3) and is
// that player's wt_sink: the scheduler's workers decode each frame straight
// into a fresh BGRA CVPixelBuffer from a pool, and the newest complete one is
// kept for -copyLatestFrame. Everything media related stays in the core; this
// file only adapts handles, errors and buffers.
//
// Threads:
// - The initialisers read and probe the source: call them on a background
//   queue, never on the main thread or a Flutter thread.
// - -setActive:, -setPriority:, -seekToFrame: and -copyLatestFrame are cheap
//   and may be called from any thread (Flutter calls -copyLatestFrame on its
//   raster thread).
// - -close waits for a frame in flight, so call it off the main thread; no
//   other call may overlap it, and the frame and error handlers are never
//   called once it returns.
// - The handlers run on scheduler workers. They must not keep the WTPlayer
//   alive (capture it weakly), or its last release could happen on a worker.
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// NSError domain for failures from the core. The error's code is the
/// wt_status value, and its localizedDescription is the status name (e.g.
/// "WT_ERR_LIMIT").
FOUNDATION_EXPORT NSErrorDomain const WTStatusErrorDomain;

/// The NSError userInfo key holding the status name as an NSString.
FOUNDATION_EXPORT NSString *const WTStatusNameKey;

/// Called on a scheduler worker each time a new frame is ready.
typedef void (^WTFrameHandler)(void);
/// Called on a scheduler worker for the first frame that fails to decode,
/// with its wt_status. Playback carries on.
typedef void (^WTErrorHandler)(int32_t status);

NS_SWIFT_NAME(WTPlayer)
@interface WTPlayer : NSObject

/// Decodes `data` (copied into the core) scaled to width x height, or the
/// source size for 0 x 0, and puts it on the scheduler, inactive. Its first
/// frame is drawn right away (then `onFrame` runs). With `loop` NO, playback
/// stops on the last frame. Fails with a WTStatusErrorDomain error when the
/// core rejects the input.
///
/// With a `cacheDirectory` and a `sourceKey` (and a non-zero size) the frames
/// are cached in a file there, named after the key and size
/// (wt_cache_file_path); the directory must exist. Pass nil for either to
/// cache nothing. Background thread.
- (nullable instancetype)initWithData:(NSData *)data
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(nullable NSString *)cacheDirectory
                            sourceKey:(nullable NSString *)sourceKey
                              onFrame:(WTFrameHandler)onFrame
                              onError:(WTErrorHandler)onError
                                error:(NSError **)error;

/// As above, from a file the core maps read-only. Background thread.
- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(nullable NSString *)cacheDirectory
                            sourceKey:(nullable NSString *)sourceKey
                              onFrame:(WTFrameHandler)onFrame
                              onError:(WTErrorHandler)onError
                                error:(NSError **)error;

/// Deletes least recently used cache files in `directory` until it fits the
/// disk budget, and stray temporaries. Returns a wt_status. Background thread.
+ (int32_t)evictCacheInDirectory:(NSString *)directory
    NS_SWIFT_NAME(evictCache(inDirectory:));

/// Applies the device tier's tunables to the process-wide scheduler, creating
/// it if needed: 1..8 workers, 1..240 fps, and a limit on concurrently playing
/// animations (0 for none). Running animations follow on the next round.
/// Returns a wt_status (WT_ERR_INVALID_ARGUMENT for a value out of range).
/// Main thread.
+ (int32_t)setSchedulerWorkerCount:(int32_t)workerCount
                            maxFps:(int32_t)maxFps
                     maxConcurrent:(int32_t)maxConcurrent
    NS_SWIFT_NAME(setSchedulerOptions(workerCount:maxFps:maxConcurrent:));

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

/// Output size every frame is scaled to, in pixels.
@property(nonatomic, readonly) uint32_t width;
@property(nonatomic, readonly) uint32_t height;
@property(nonatomic, readonly) uint32_t frameCount;

/// Whether a frame is ready for -copyLatestFrame. Any thread.
@property(nonatomic, readonly) BOOL hasFrame;

/// The newest complete frame, retained for the caller, or NULL before the
/// first one and after -close. Any thread.
- (nullable CVPixelBufferRef)copyLatestFrame CF_RETURNS_RETAINED;

/// Starts or stops playback; a stopped player holds its frame. Any thread.
- (void)setActive:(BOOL)active;

/// The on-screen area of the widgets showing this player, in physical
/// pixels: its rank under the scheduler's max_concurrent limit. Any thread.
- (void)setPriority:(uint64_t)priority;

/// Shows `frame` as soon as a worker is free, playing or not; a playing
/// player carries on from there and an ended one plays again. Returns a
/// wt_status: WT_ERR_INVALID_ARGUMENT when frame >= frameCount or after
/// -close. Any thread.
- (int32_t)seekToFrame:(uint32_t)frame NS_SWIFT_NAME(seek(toFrame:));

/// Whether the player's own buffers are on screen (T4.2). With NO (its
/// texture was retired: the animation draws into an atlas, or its atlas was
/// disposed), a frame meant for its own buffers parks the player instead of
/// being decoded; YES draws the frame that should be showing. Any thread.
- (void)setHasTarget:(BOOL)hasTarget;

/// Frees the newest frame and the pool's idle buffers, once nothing shows
/// the player's own texture (it is in an atlas). Any thread.
- (void)dropFrames;

/// Takes the player off the scheduler, waiting for a frame in flight, and
/// frees the decoder and every buffer. Idempotent. Not on the main thread.
/// A player in an atlas leaves it; detach it first if the atlas may be
/// closed at the same time (wt_atlas_destroy must not overlap it).
- (void)close;

/// The static name of a wt_status, such as "WT_ERR_DECODE".
+ (NSString *)nameForStatus:(int32_t)status;

@end

NS_ASSUME_NONNULL_END
