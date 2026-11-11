#import "WTAtlas.h"

#import <IOSurface/IOSurfaceRef.h>

#include <mutex>

#import "WTPlayer+Internal.h"
#include "wt_api.h"

#if !__has_feature(objc_arc)
#error "WTAtlas.mm must be compiled with ARC"
#endif

namespace {

// Buffers alive at once per page: the one Flutter holds, the newest, one
// being written and a spare. With all of them in use, acquire answers
// WT_BUSY and the core presents again a tick later.
constexpr int kMaxPageBuffers = 4;

}  // namespace

@interface WTAtlasPage ()
// The page as a wt_atlas_target; its context is the page, unretained.
- (wt_atlas_target)target;
@end

static wt_status PageAcquire(void *context, wt_buffer *out, uint64_t *out_id);
static void PagePresent(void *context, wt_status status);

@implementation WTAtlasPage {
  WTPageFrameHandler _onFrame;
  CVPixelBufferPoolRef _pool;
  NSDictionary *_poolAux;
  std::mutex _latestMu;
  CVPixelBufferRef _latest;  // Guarded by _latestMu (+1).
  CVPixelBufferRef _pending;  // Worker only: the buffer being written.
}

- (nullable instancetype)initWithWidth:(uint32_t)width
                                height:(uint32_t)height
                               onFrame:(WTPageFrameHandler)onFrame {
  self = [super init];
  if (!self) return nil;
  NSDictionary *attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferWidthKey : @(width),
    (id)kCVPixelBufferHeightKey : @(height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
  };
  if (width == 0 || height == 0 ||
      CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs,
                              &_pool) != kCVReturnSuccess) {
    return nil;
  }
  _width = width;
  _height = height;
  _onFrame = [onFrame copy];
  _poolAux =
      @{(id)kCVPixelBufferPoolAllocationThresholdKey : @(kMaxPageBuffers)};
  return self;
}

- (void)dealloc {
  if (_latest != NULL) CVPixelBufferRelease(_latest);
  if (_pool != NULL) CVPixelBufferPoolRelease(_pool);
}

- (CVPixelBufferRef)copyLatestFrame {
  std::lock_guard<std::mutex> lk(_latestMu);
  if (_latest != NULL) CVPixelBufferRetain(_latest);
  return _latest;
}

- (wt_atlas_target)target {
  wt_atlas_target t = {};
  // Unretained: WTAtlas keeps the page alive while it is a target.
  t.context = (__bridge void *)self;
  t.acquire = &PageAcquire;
  t.present = &PagePresent;
  return t;
}

// --- wt_atlas_target (scheduler workers, one call at a time) ---------------

static wt_status PageAcquire(void *context, wt_buffer *out, uint64_t *out_id) {
  __unsafe_unretained WTAtlasPage *page = (__bridge WTAtlasPage *)context;
  CVPixelBufferRef buffer = NULL;
  const CVReturn rv = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
      NULL, page->_pool, (__bridge CFDictionaryRef)page->_poolAux, &buffer);
  if (rv == kCVReturnWouldExceedAllocationThreshold) return WT_BUSY;
  if (rv != kCVReturnSuccess || buffer == NULL) return WT_ERR_NO_MEMORY;
  if (CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
      CVPixelBufferIsPlanar(buffer) ||
      CVPixelBufferGetWidth(buffer) != page->_width ||
      CVPixelBufferGetHeight(buffer) != page->_height ||
      CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
    CVPixelBufferRelease(buffer);
    return WT_ERR_INTERNAL;
  }
  auto *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  if (base == NULL || stride > UINT32_MAX ||
      stride > SIZE_MAX / page->_height) {
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    CVPixelBufferRelease(buffer);
    return WT_ERR_INTERNAL;
  }
  out->pixels = base;
  out->size = stride * page->_height;
  out->stride = static_cast<uint32_t>(stride);
  out->format = WT_PIXEL_FORMAT_BGRA8888;
  // The memory stats count distinct IOSurfaces, as spike C did.
  IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
  *out_id = surface != NULL ? IOSurfaceGetID(surface) : 0;
  page->_pending = buffer;
  return WT_OK;
}

static void PagePresent(void *context, wt_status status) {
  __unsafe_unretained WTAtlasPage *page = (__bridge WTAtlasPage *)context;
  CVPixelBufferRef buffer = page->_pending;
  page->_pending = NULL;
  CVPixelBufferUnlockBaseAddress(buffer, 0);
  if (status != WT_OK) {
    CVPixelBufferRelease(buffer);
    return;
  }
  CVPixelBufferRef old;
  {
    std::lock_guard<std::mutex> lk(page->_latestMu);
    old = page->_latest;
    page->_latest = buffer;  // Takes the pool's +1.
  }
  if (old != NULL) CVPixelBufferRelease(old);
  page->_onFrame();
}

@end

@implementation WTAtlas {
  wt_atlas *_atlas;
  uint32_t _width;
  // The current target of each page, kept alive while the core may call it
  // (NSNull: none).
  NSMutableArray *_targets;
}

- (nullable instancetype)initWithWidth:(uint32_t)width
                             maxHeight:(uint32_t)maxHeight
                              maxPages:(uint32_t)maxPages
                                 error:(NSError **)error {
  self = [super init];
  if (!self) return nil;
  wt_atlas_options options;
  wt_atlas_options_init_default(&options);
  options.width = width;
  options.max_height = maxHeight;
  options.max_pages = maxPages;
  options.format = WT_PIXEL_FORMAT_BGRA8888;
  wt_sched *sched = WTSharedScheduler();
  const wt_status status = sched == nullptr
                               ? WT_ERR_NO_MEMORY
                               : wt_atlas_create(sched, &options, &_atlas);
  if (status != WT_OK) {
    if (error) {
      NSString *name = [WTPlayer nameForStatus:status];
      *error = [NSError errorWithDomain:WTStatusErrorDomain
                                   code:status
                               userInfo:@{
                                 NSLocalizedDescriptionKey : name,
                                 WTStatusNameKey : name,
                               }];
    }
    return nil;
  }
  _width = width & ~15u;
  _targets = [NSMutableArray array];
  return self;
}

- (void)dealloc {
  [self close];
}

- (int32_t)attachPlayer:(WTPlayer *)player slot:(WTAtlasSlot *)slot {
  if (_atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  wt_atlas *atlas = _atlas;
  __block wt_atlas_slot s = {};
  const int32_t status = [player withPlayerHandle:^int32_t(wt_player *p) {
    return p == nullptr ? WT_ERR_INVALID_ARGUMENT
                        : wt_atlas_attach(atlas, p, &s);
  }];
  if (status == WT_OK) {
    slot->page = s.page;
    slot->x = s.x;
    slot->y = s.y;
    slot->width = s.width;
    slot->height = s.height;
  }
  return status;
}

- (int32_t)detachPlayer:(WTPlayer *)player {
  if (_atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  wt_atlas *atlas = _atlas;
  return [player withPlayerHandle:^int32_t(wt_player *p) {
    return p == nullptr ? WT_ERR_INVALID_ARGUMENT : wt_atlas_detach(atlas, p);
  }];
}

- (int32_t)shownPageForPlayer:(WTPlayer *)player {
  if (_atlas == nullptr) return -1;
  wt_atlas *atlas = _atlas;
  return [player withPlayerHandle:^int32_t(wt_player *p) {
    return p == nullptr ? -1 : wt_atlas_shown_page(atlas, p);
  }];
}

- (uint32_t)pageCount {
  return wt_atlas_page_count(_atlas);
}

- (int32_t)getPage:(uint32_t)page info:(WTAtlasPageInfo *)info {
  wt_atlas_page_info i;
  const wt_status status = wt_atlas_get_page(_atlas, page, &i);
  if (status != WT_OK) return status;
  info->width = i.width;
  info->usedHeight = i.used_height;
  info->wantedHeight = i.wanted_height;
  info->height = i.height;
  info->hasTarget = i.has_target;
  info->liveSlots = i.live_slots;
  info->buffers = i.buffers;
  info->bufferStride = i.buffer_stride;
  info->bufferBytes = i.buffer_bytes;
  info->cpuBytes = i.cpu_bytes;
  info->presents = i.presents;
  info->presentFailures = i.present_failures;
  return WT_OK;
}

- (int32_t)setPage:(uint32_t)page
            target:(WTAtlasPage *)target
            height:(uint32_t)height {
  if (_atlas == nullptr) return WT_ERR_INVALID_ARGUMENT;
  if (target != nil && (target.width != _width || target.height != height)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  wt_atlas_target t = {};
  if (target != nil) t = [target target];
  const wt_status status =
      wt_atlas_set_target(_atlas, page, target != nil ? &t : nullptr, height);
  if (status != WT_OK) return status;
  // The core is done with the old target: drop our reference to it.
  while (_targets.count <= page) [_targets addObject:[NSNull null]];
  _targets[page] = target != nil ? target : [NSNull null];
  return WT_OK;
}

- (void)close {
  if (_atlas == nullptr) return;
  wt_atlas_destroy(_atlas);  // No target callback runs after this.
  _atlas = nullptr;
  [_targets removeAllObjects];
}

@end
