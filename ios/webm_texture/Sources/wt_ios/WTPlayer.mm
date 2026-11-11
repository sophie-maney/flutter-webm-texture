#import "WTPlayer.h"

#include <limits.h>
#include <pthread.h>

#include <atomic>
#include <memory>
#include <mutex>

#import "WTPlayer+Internal.h"

#include "wt_api.h"

#if !__has_feature(objc_arc)
#error "WTPlayer.mm must be compiled with ARC"
#endif

NSErrorDomain const WTStatusErrorDomain = @"WTStatusErrorDomain";
NSString *const WTStatusNameKey = @"WTStatusName";

namespace {

struct SourceDeleter {
  void operator()(wt_source *s) const { wt_source_destroy(s); }
};
struct AnimDeleter {
  void operator()(wt_anim *a) const { wt_anim_destroy(a); }
};
using SourcePtr = std::unique_ptr<wt_source, SourceDeleter>;
using AnimPtr = std::unique_ptr<wt_anim, AnimDeleter>;

// Buffers alive at once per player: the one Flutter holds, the one kept as
// the latest, one being written and one spare. With all of them still in
// use, acquire answers WT_BUSY and the scheduler tries again next tick
// rather than growing the pool.
constexpr int kMaxBuffers = 4;

NSError *StatusError(wt_status status) {
  NSString *name = [WTPlayer nameForStatus:status];
  return [NSError errorWithDomain:WTStatusErrorDomain
                             code:status
                         userInfo:@{
                           NSLocalizedDescriptionKey : name,
                           WTStatusNameKey : name,
                         }];
}

// Scheduler threads decode for what is on screen: the same QoS the T2.3
// per-animation queues had.
void SetThreadQos(void *, const char *) {
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
}


CVPixelBufferPoolRef MakePool(uint32_t width, uint32_t height) {
  NSDictionary *attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferWidthKey : @(width),
    (id)kCVPixelBufferHeightKey : @(height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
  };
  CVPixelBufferPoolRef pool = NULL;
  if (CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs,
                              &pool) != kCVReturnSuccess) {
    return NULL;
  }
  return pool;
}

}  // namespace

// The process-wide scheduler, or NULL if it couldn't start. It lives as long
// as the process. T3.4 sets its options from the device tier; atlases
// (WTAtlas.mm) use it too.
wt_sched *WTSharedScheduler(void) {
  static wt_sched *sched = nullptr;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    wt_sched_hooks hooks = {};
    hooks.thread_start = &SetThreadQos;
    if (wt_sched_create(nullptr, &hooks, &sched) != WT_OK) sched = nullptr;
  });
  return sched;
}

// The wt_sink callbacks, defined inside the @implementation for ivar access.
static wt_status AcquireBuffer(void *context, wt_buffer *out);
static void ReleaseBuffer(void *context, wt_status status,
                          const wt_frame_info *frame);
static void ReportError(void *context, wt_status status);

@implementation WTPlayer {
  WTFrameHandler _onFrame;
  WTErrorHandler _onError;
  CVPixelBufferPoolRef _pool;
  NSDictionary *_poolAux;

  // Guards _player: -close takes it away under the lock, so no call on it
  // can overlap wt_player_destroy.
  std::mutex _playerMu;
  wt_player *_player;

  // Guards _latest, the newest complete frame (+1).
  std::mutex _latestMu;
  CVPixelBufferRef _latest;

  // Worker only (one frame in flight): the locked buffer being written.
  CVPixelBufferRef _pending;

  // Whether frames for the player's own buffers are shown (setHasTarget:).
  std::atomic<bool> _hasTarget;
}

- (nullable instancetype)initWithData:(NSData *)data
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(NSString *)cacheDirectory
                            sourceKey:(NSString *)sourceKey
                              onFrame:(WTFrameHandler)onFrame
                              onError:(WTErrorHandler)onError
                                error:(NSError **)error {
  wt_source *raw = nullptr;
  // An empty NSData may have a NULL bytes pointer; the core rejects size 0.
  wt_status st = wt_source_create_memory(
      static_cast<const uint8_t *>(data.bytes), data.length, nullptr, &raw);
  return [self initWithSource:raw
                       status:st
                        width:width
                       height:height
                         loop:loop
               cacheDirectory:cacheDirectory
                    sourceKey:sourceKey
                      onFrame:onFrame
                      onError:onError
                        error:error];
}

- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(NSString *)cacheDirectory
                            sourceKey:(NSString *)sourceKey
                              onFrame:(WTFrameHandler)onFrame
                              onError:(WTErrorHandler)onError
                                error:(NSError **)error {
  wt_source *raw = nullptr;
  wt_status st = wt_source_create_file(path.fileSystemRepresentation, nullptr,
                                       &raw);
  return [self initWithSource:raw
                       status:st
                        width:width
                       height:height
                         loop:loop
               cacheDirectory:cacheDirectory
                    sourceKey:sourceKey
                      onFrame:onFrame
                      onError:onError
                        error:error];
}

// Takes ownership of rawSource (NULL when creating it failed with status).
- (nullable instancetype)initWithSource:(wt_source *)rawSource
                                 status:(wt_status)status
                                  width:(uint32_t)width
                                 height:(uint32_t)height
                                   loop:(BOOL)loop
                         cacheDirectory:(NSString *)cacheDirectory
                              sourceKey:(NSString *)sourceKey
                                onFrame:(WTFrameHandler)onFrame
                                onError:(WTErrorHandler)onError
                                  error:(NSError **)error {
  SourcePtr source(rawSource);
  self = [super init];
  if (!self) return nil;
  wt_anim *raw = nullptr;
  if (status == WT_OK) {
    // The animation takes its own reference; `source` drops ours on return.
    // The output size is only known up front for a real bucket, so a 0 x 0
    // animation isn't cached.
    char path[PATH_MAX] = "";
    const bool cache = cacheDirectory != nil && sourceKey != nil && width > 0 &&
                       height > 0 &&
                       wt_cache_file_path(cacheDirectory.fileSystemRepresentation,
                                          sourceKey.UTF8String, width, height,
                                          path, sizeof(path)) == WT_OK;
    status = wt_anim_create_cached(source.get(), width, height,
                                   cache ? path : nullptr, &raw);
  }
  AnimPtr anim(raw);
  wt_info info = {};
  uint32_t outW = 0;
  uint32_t outH = 0;
  if (status == WT_OK) {
    status = wt_anim_get_info(anim.get(), &info, &outW, &outH);
  }
  if (status == WT_OK && (outW == 0 || outH == 0 || info.frame_count == 0)) {
    status = WT_ERR_INTERNAL;
  }
  if (status == WT_OK) {
    _pool = MakePool(outW, outH);
    if (_pool == NULL) status = WT_ERR_NO_MEMORY;
  }
  wt_sched *sched = WTSharedScheduler();
  if (status == WT_OK && sched == nullptr) status = WT_ERR_NO_MEMORY;
  if (status != WT_OK) {
    if (error) *error = StatusError(status);
    return nil;
  }
  _width = outW;
  _height = outH;
  _frameCount = info.frame_count;
  _onFrame = [onFrame copy];
  _onError = [onError copy];
  _poolAux = @{(id)kCVPixelBufferPoolAllocationThresholdKey : @(kMaxBuffers)};
  _hasTarget.store(true);

  wt_sink sink = {};
  sink.context = (__bridge void *)self;
  sink.acquire = &AcquireBuffer;
  sink.release = &ReleaseBuffer;
  sink.error = &ReportError;
  wt_player *player = nullptr;
  // The workers may call the sink from here on: everything it reads is set.
  status = wt_player_create(sched, anim.get(), loop, &sink, &player);
  if (status != WT_OK) {
    if (error) *error = StatusError(status);
    return nil;  // dealloc frees the pool; `anim` is still ours.
  }
  anim.release();  // The player owns it now.
  std::lock_guard<std::mutex> lk(_playerMu);
  _player = player;
  return self;
}

- (void)dealloc {
  [self close];
  if (_pool != NULL) CVPixelBufferPoolRelease(_pool);
}

+ (int32_t)evictCacheInDirectory:(NSString *)directory {
  return wt_cache_evict(directory.fileSystemRepresentation, nullptr);
}

+ (int32_t)setSchedulerWorkerCount:(int32_t)workerCount
                            maxFps:(int32_t)maxFps
                     maxConcurrent:(int32_t)maxConcurrent {
  if (workerCount < 1 || maxFps < 1 || maxConcurrent < 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_sched *sched = WTSharedScheduler();
  if (sched == nullptr) return WT_ERR_NO_MEMORY;
  wt_sched_options options;
  wt_sched_options_init_default(&options);
  options.worker_count = static_cast<uint32_t>(workerCount);
  options.max_fps = static_cast<uint32_t>(maxFps);
  options.max_concurrent = static_cast<uint32_t>(maxConcurrent);
  return wt_sched_set_options(sched, &options);
}

- (BOOL)hasFrame {
  std::lock_guard<std::mutex> lk(_latestMu);
  return _latest != NULL;
}

- (CVPixelBufferRef)copyLatestFrame {
  std::lock_guard<std::mutex> lk(_latestMu);
  if (_latest != NULL) CVPixelBufferRetain(_latest);
  return _latest;
}

- (void)setActive:(BOOL)active {
  std::lock_guard<std::mutex> lk(_playerMu);
  if (_player != nullptr) wt_player_set_active(_player, active);
}

- (void)setPriority:(uint64_t)priority {
  std::lock_guard<std::mutex> lk(_playerMu);
  if (_player != nullptr) wt_player_set_priority(_player, priority);
}

- (int32_t)seekToFrame:(uint32_t)frame {
  std::lock_guard<std::mutex> lk(_playerMu);
  if (_player == nullptr) return WT_ERR_INVALID_ARGUMENT;
  return wt_player_seek(_player, frame);
}

- (void)setHasTarget:(BOOL)hasTarget {
  _hasTarget.store(hasTarget);
  if (!hasTarget) return;
  std::lock_guard<std::mutex> lk(_playerMu);
  // Lets a parked player draw again (it got WT_NO_TARGET meanwhile).
  if (_player != nullptr) wt_player_redraw(_player);
}

- (void)dropFrames {
  CVPixelBufferRef latest;
  {
    std::lock_guard<std::mutex> lk(_latestMu);
    latest = _latest;
    _latest = NULL;
  }
  if (latest != NULL) CVPixelBufferRelease(latest);
  if (_pool != NULL) {
    CVPixelBufferPoolFlush(_pool, kCVPixelBufferPoolFlushExcessBuffers);
  }
}

- (int32_t)withPlayerHandle:(int32_t (^)(wt_player *))block {
  std::lock_guard<std::mutex> lk(_playerMu);
  return block(_player);
}

- (void)close {
  wt_player *player;
  {
    std::lock_guard<std::mutex> lk(_playerMu);
    player = _player;
    _player = nullptr;
  }
  // Waits for a frame in flight; no sink callback runs after this.
  wt_player_destroy(player);
  CVPixelBufferRef latest;
  {
    std::lock_guard<std::mutex> lk(_latestMu);
    latest = _latest;
    _latest = NULL;
  }
  if (latest != NULL) CVPixelBufferRelease(latest);
  if (player != nullptr && _pool != NULL) {
    CVPixelBufferPoolFlush(_pool, kCVPixelBufferPoolFlushExcessBuffers);
  }
}

+ (NSString *)nameForStatus:(int32_t)status {
  return @(wt_status_string(status));
}

// --- wt_sink (scheduler workers, one call at a time) -------------------------
//
// `context` is the player, unretained: -close (also run by dealloc) waits for
// these callbacks, so the object outlives them, and retaining it here could
// resurrect one that is being deallocated.

static wt_status AcquireBuffer(void *context, wt_buffer *out) {
  __unsafe_unretained WTPlayer *player = (__bridge WTPlayer *)context;
  // Nothing shows these buffers: wait for setHasTarget:YES.
  if (!player->_hasTarget.load()) return WT_NO_TARGET;
  CVPixelBufferRef buffer = NULL;
  const CVReturn rv = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
      NULL, player->_pool, (__bridge CFDictionaryRef)player->_poolAux, &buffer);
  if (rv == kCVReturnWouldExceedAllocationThreshold) return WT_BUSY;
  if (rv != kCVReturnSuccess || buffer == NULL) return WT_ERR_NO_MEMORY;
  if (CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
      CVPixelBufferIsPlanar(buffer) ||
      CVPixelBufferGetWidth(buffer) != player->_width ||
      CVPixelBufferGetHeight(buffer) != player->_height ||
      CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
    CVPixelBufferRelease(buffer);
    return WT_ERR_INTERNAL;
  }
  auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  // stride * height is the plane CoreVideo allocated; the core also checks
  // stride >= width * 4 and the size against its own minimum.
  if (base == NULL || stride > UINT32_MAX ||
      stride > SIZE_MAX / player->_height) {
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    CVPixelBufferRelease(buffer);
    return WT_ERR_INTERNAL;
  }
  out->pixels = base;
  out->size = stride * player->_height;
  out->stride = static_cast<uint32_t>(stride);
  out->format = WT_PIXEL_FORMAT_BGRA8888;
  player->_pending = buffer;
  return WT_OK;
}

static void ReleaseBuffer(void *context, wt_status status,
                          const wt_frame_info *) {
  __unsafe_unretained WTPlayer *player = (__bridge WTPlayer *)context;
  CVPixelBufferRef buffer = player->_pending;
  player->_pending = NULL;
  CVPixelBufferUnlockBaseAddress(buffer, 0);
  if (status != WT_OK) {
    // Possibly a partial frame: keep showing the previous one.
    CVPixelBufferRelease(buffer);
    return;
  }
  CVPixelBufferRef old;
  {
    std::lock_guard<std::mutex> lk(player->_latestMu);
    old = player->_latest;
    player->_latest = buffer;  // Takes the pool's +1.
  }
  if (old != NULL) CVPixelBufferRelease(old);
  player->_onFrame();
}

static void ReportError(void *context, wt_status status) {
  __unsafe_unretained WTPlayer *player = (__bridge WTPlayer *)context;
  player->_onError(status);
}

@end
