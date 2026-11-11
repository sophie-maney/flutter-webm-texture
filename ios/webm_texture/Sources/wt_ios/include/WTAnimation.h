// WTAnimation: the ObjC++ shim between the Swift texture glue and wt_api.h.
//
// It owns one wt_anim (and, briefly, the wt_source it was made from) and
// decodes frames straight into a locked BGRA CVPixelBuffer. Everything media
// related stays in the core; this file only adapts handles, errors and
// buffers.
//
// Threading: WTAnimation is not thread-safe, like the wt_anim it wraps. Every
// method that decodes (the initialisers and -renderNextFrameInto:...) reads
// files or decodes, so call it on a background queue, never on the main
// thread or a Flutter thread. The Swift glue gives each animation one serial
// queue and calls everything there. -close and dealloc are cheap.
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// NSError domain for failures from the core. The error's code is the
/// wt_status value, and its localizedDescription is the status name (e.g.
/// "WT_ERR_LIMIT").
FOUNDATION_EXPORT NSErrorDomain const WTStatusErrorDomain;

/// The NSError userInfo key holding the status name as an NSString.
FOUNDATION_EXPORT NSString *const WTStatusNameKey;

NS_SWIFT_NAME(WTAnimation)
@interface WTAnimation : NSObject

/// Decodes `data` (copied into the core) scaled to width x height, or the
/// source size for 0 x 0. With `loop` NO, -renderNextFrameInto: stops after
/// the last frame instead of wrapping. Fails with a WTStatusErrorDomain error
/// when the core rejects the input.
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
                                error:(NSError **)error;

/// As above, from a file the core maps read-only. Background thread.
- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(nullable NSString *)cacheDirectory
                            sourceKey:(nullable NSString *)sourceKey
                                error:(NSError **)error;

/// Deletes least recently used cache files in `directory` until it fits the
/// disk budget, and stray temporaries. Returns a wt_status. Background thread.
+ (int32_t)evictCacheInDirectory:(NSString *)directory
    NS_SWIFT_NAME(evictCache(inDirectory:));

- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

/// Output size every frame is scaled to, in pixels.
@property(nonatomic, readonly) uint32_t width;
@property(nonatomic, readonly) uint32_t height;
@property(nonatomic, readonly) uint32_t frameCount;
@property(nonatomic, readonly) BOOL loops;

/// Decodes the frame at the cursor into `buffer` and advances the cursor.
/// After the last frame, a looping animation wraps to frame 0; a
/// non-looping one returns WT_END (1) and writes nothing until -seekToFrame:.
///
/// `buffer` must be kCVPixelFormatType_32BGRA and exactly width x height;
/// anything else returns WT_ERR_INVALID_ARGUMENT without decoding. The
/// buffer is locked for the duration of the call.
///
/// Returns a wt_status: 0 (WT_OK) when `buffer` holds a complete frame. On a
/// negative status the cursor still advanced (one broken frame doesn't stall
/// playback) but `buffer` may hold a partial frame, so don't show it.
/// `outDurationMs` receives how long to show the frame, or 0 when unknown.
/// Background thread. Fails with WT_ERR_INVALID_ARGUMENT after -close.
- (int32_t)renderNextFrameInto:(CVPixelBufferRef)buffer
                    durationMs:(uint32_t *_Nullable)outDurationMs
    NS_SWIFT_NAME(renderNextFrame(into:durationMs:));

/// Moves the cursor so the next -renderNextFrameInto: returns `frame`.
/// Cheap: the decoding happens in that call. Returns a wt_status,
/// WT_ERR_INVALID_ARGUMENT when frame >= frameCount or after -close.
/// Background thread.
- (int32_t)seekToFrame:(uint32_t)frame NS_SWIFT_NAME(seek(toFrame:));

/// Destroys the decoder now instead of at dealloc. Idempotent.
- (void)close;

/// The static name of a wt_status, such as "WT_ERR_DECODE".
+ (NSString *)nameForStatus:(int32_t)status;

@end

NS_ASSUME_NONNULL_END
