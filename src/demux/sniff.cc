#include "demux/sniff.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wt {
namespace {

bool StartsWith(ByteView bytes, size_t offset, const char* magic, size_t len) {
  return bytes.size >= offset && bytes.size - offset >= len &&
         std::memcmp(bytes.data + offset, magic, len) == 0;
}

}  // namespace

wt_format Sniff(ByteView bytes) {
  if (StartsWith(bytes, 0, "\x1A\x45\xDF\xA3", 4)) return WT_FORMAT_WEBM;
  if (StartsWith(bytes, 0, "RIFF", 4) && StartsWith(bytes, 8, "WEBP", 4)) {
    return WT_FORMAT_WEBP;
  }
  if (StartsWith(bytes, 0, "GIF87a", 6) || StartsWith(bytes, 0, "GIF89a", 6)) {
    return WT_FORMAT_GIF;
  }
  return WT_FORMAT_UNKNOWN;
}

}  // namespace wt
