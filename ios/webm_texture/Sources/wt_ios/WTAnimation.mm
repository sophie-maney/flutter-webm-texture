#import "WTAnimation.h"

#include <limits.h>

#include <memory>

#include "wt_api.h"

#if !__has_feature(objc_arc)
#error "WTAnimation.mm must be compiled with ARC"
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

NSError *StatusError(wt_status status) {
  NSString *name = [WTAnimation nameForStatus:status];
  return [NSError errorWithDomain:WTStatusErrorDomain
                             code:status
                         userInfo:@{
                           NSLocalizedDescriptionKey : name,
                           WTStatusNameKey : name,
                         }];
}

}  // namespace

@implementation WTAnimation {
  AnimPtr _anim;
}

- (nullable instancetype)initWithData:(NSData *)data
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(NSString *)cacheDirectory
                            sourceKey:(NSString *)sourceKey
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
                        error:error];
}

- (nullable instancetype)initWithPath:(NSString *)path
                                width:(uint32_t)width
                               height:(uint32_t)height
                                 loop:(BOOL)loop
                       cacheDirectory:(NSString *)cacheDirectory
                            sourceKey:(NSString *)sourceKey
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
  if (status != WT_OK || outW == 0 || outH == 0 || info.frame_count == 0) {
    if (error) {
      *error = StatusError(status != WT_OK ? status : WT_ERR_INTERNAL);
    }
    return nil;
  }
  _anim = std::move(anim);
  _width = outW;
  _height = outH;
  _frameCount = info.frame_count;
  _loops = loop;
  return self;
}

+ (int32_t)evictCacheInDirectory:(NSString *)directory {
  return wt_cache_evict(directory.fileSystemRepresentation, nullptr);
}

- (int32_t)renderNextFrameInto:(CVPixelBufferRef)buffer
                    durationMs:(uint32_t *)outDurationMs {
  if (outDurationMs) *outDurationMs = 0;
  if (!_anim || !buffer ||
      CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
      CVPixelBufferIsPlanar(buffer) ||
      CVPixelBufferGetWidth(buffer) != _width ||
      CVPixelBufferGetHeight(buffer) != _height) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
    return WT_ERR_INTERNAL;
  }
  auto *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  wt_status st = WT_ERR_INTERNAL;
  // stride * height is the plane CoreVideo allocated; the core also checks
  // stride >= width * 4 and the size against its own minimum.
  if (dst && stride <= UINT32_MAX && stride <= SIZE_MAX / _height) {
    const size_t size = stride * _height;
    wt_frame_info frame = {};
    st = wt_anim_next_frame(_anim.get(), dst, size, (uint32_t)stride,
                            WT_PIXEL_FORMAT_BGRA8888, &frame);
    if (st == WT_END && _loops) {
      st = wt_anim_seek(_anim.get(), 0);
      if (st == WT_OK) {
        st = wt_anim_next_frame(_anim.get(), dst, size, (uint32_t)stride,
                                WT_PIXEL_FORMAT_BGRA8888, &frame);
      }
      // A second WT_END means the core has nothing to play; report it as
      // the bug it would be rather than as success.
      if (st == WT_END) st = WT_ERR_INTERNAL;
    }
    if (outDurationMs) *outDurationMs = frame.duration_ms;
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);
  return st;
}

- (int32_t)seekToFrame:(uint32_t)frame {
  if (!_anim) return WT_ERR_INVALID_ARGUMENT;
  return wt_anim_seek(_anim.get(), frame);
}

- (void)close {
  _anim.reset();
}

+ (NSString *)nameForStatus:(int32_t)status {
  return @(wt_status_string(status));
}

@end
