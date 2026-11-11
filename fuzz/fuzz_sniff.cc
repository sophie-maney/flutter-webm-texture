// Fuzzes format sniffing (src/demux/sniff.*) and the bounds checks of the
// byte source every demuxer reads through (src/demux/source.*).
//
// Sniff is checked against an independent byte-by-byte oracle. Slice/Read
// take offsets and lengths decoded from the input itself, the way a demuxer
// takes them from a hostile file, including values near UINT64_MAX.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>

#include "demux/sniff.h"
#include "demux/source.h"
#include "wt_api.h"

namespace {

wt_format Oracle(const uint8_t* d, size_t n) {
  if (n >= 4 && d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) {
    return WT_FORMAT_WEBM;
  }
  if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' &&
      d[8] == 'W' && d[9] == 'E' && d[10] == 'B' && d[11] == 'P') {
    return WT_FORMAT_WEBP;
  }
  if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8' &&
      (d[4] == '7' || d[4] == '9') && d[5] == 'a') {
    return WT_FORMAT_GIF;
  }
  return WT_FORMAT_UNKNOWN;
}

uint64_t Load64(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (wt::Sniff(wt::ByteView{data, size}) != Oracle(data, size)) {
    __builtin_trap();
  }

  std::unique_ptr<wt::ByteSource> source;
  if (wt::ByteSource::CreateMemory(data, size, size, &source) != WT_OK) {
    __builtin_trap();
  }
  if (source->size() != size) __builtin_trap();
  if (wt::Sniff(source->whole()) != Oracle(data, size)) __builtin_trap();

  // Treat the first 16 bytes as an (offset, length) pair; small lengths
  // are folded so that in-range reads actually happen.
  if (size >= 16) {
    const uint64_t offset = Load64(data);
    const uint64_t raw = Load64(data + 8);
    for (uint64_t length : {raw, raw % 64, raw % (size + 2)}) {
      const bool in_range = offset <= size && length <= size - offset;
      wt::ByteView view{data, 1};
      const bool ok = source->Slice(offset, length, &view);
      if (ok != in_range) __builtin_trap();
      if (ok) {
        if (view.size != length) __builtin_trap();
        if (length != 0 && std::memcmp(view.data, data + offset, length) != 0) {
          __builtin_trap();
        }
      } else if (view.data != nullptr || view.size != 0) {
        __builtin_trap();
      }

      if (length > 64) continue;
      uint8_t buffer[64];
      std::memset(buffer, 0xEE, sizeof(buffer));
      const bool read =
          source->Read(offset, static_cast<size_t>(length), buffer);
      if (read != in_range) __builtin_trap();
      for (size_t i = 0; i < sizeof(buffer); ++i) {
        const bool written = read && i < length;
        if (written ? buffer[i] != data[offset + i] : buffer[i] != 0xEE) {
          __builtin_trap();
        }
      }
    }
  }

  // The public path: the format a source sniffs as decides the demuxer.
  // GIF must be rejected without parsing until T5.2 adds it. WebM and WebP
  // probing are fuzz_probe's and fuzz_webp's job.
  const wt_format format = Oracle(data, size);
  if (format == WT_FORMAT_WEBM || format == WT_FORMAT_WEBP) return 0;
  wt_source* api = nullptr;
  if (wt_source_create_memory(data, size, nullptr, &api) != WT_OK) {
    return 0;  // Empty input or over max_bytes.
  }
  wt_info info;
  const wt_status status = wt_probe(api, &info);
  if (format == WT_FORMAT_UNKNOWN && status != WT_ERR_INVALID_DATA) {
    __builtin_trap();
  }
  if (format == WT_FORMAT_GIF && status != WT_ERR_UNSUPPORTED) {
    __builtin_trap();
  }
  wt_source_destroy(api);
  return 0;
}
