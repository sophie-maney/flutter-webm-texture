// Fuzzes the frame cache reader (T3.1): arbitrary bytes as a .wsc file ->
// WscReader::Open -> ReadFrame on every frame.
//
// Each input runs twice: as is, and with every CRC it covers recomputed, so
// mutations also reach the table, layout and LZ4 checks behind the CRCs. The
// expectation passed to Open comes from the input's own header, as if the
// caller's source matched it. Whatever opens must describe what was
// expected and stay inside the limits.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "cache/crc32.h"
#include "cache/wsc.h"
#include "convert/yuva.h"
#include "demux/source.h"
#include "wt_api.h"

namespace {

uint32_t Get32(const uint8_t* p) {
  return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 |
         uint32_t{p[3]} << 24;
}

uint64_t Get64(const uint8_t* p) {
  return uint64_t{Get32(p)} | uint64_t{Get32(p + 4)} << 32;
}

void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// Recomputes the frame, table and header CRCs wherever their ranges are
// inside the file (offsets from docs/cache-format.md).
void FixCrcs(std::vector<uint8_t>* file) {
  uint8_t* f = file->data();
  const size_t size = file->size();
  if (size < wt::kWscHeaderSize) return;
  const uint64_t count = Get32(f + 24);
  if (count <= (size - wt::kWscHeaderSize) / wt::kWscEntrySize) {
    uint8_t* table = f + wt::kWscHeaderSize;
    for (uint64_t i = 0; i < count; ++i) {
      uint8_t* e = table + i * wt::kWscEntrySize;
      const uint64_t offset = Get64(e);
      const uint64_t len = Get32(e + 16);
      if (offset <= size && len <= size - offset) {
        Put32(e + 20, wt::Crc32(0, f + offset, static_cast<size_t>(len)));
      }
    }
    Put32(f + 56,
          wt::Crc32(0, table, static_cast<size_t>(count) * wt::kWscEntrySize));
  }
  Put32(f + 60, wt::Crc32(0, f, 60));
}

void Run(const std::vector<uint8_t>& file, const wt_limits& limits) {
  wt::WscParams expect;
  if (file.size() >= wt::kWscHeaderSize) {
    const uint8_t* h = file.data();
    expect.width = Get32(h + 16);
    expect.height = Get32(h + 20);
    expect.frame_count = Get32(h + 24);
    expect.has_alpha = (Get32(h + 28) & 1) != 0;
    expect.source_crc = Get32(h + 36);
    expect.source_size = Get64(h + 40);
  }
  // The caller's own expectation is always within the limits.
  if (expect.width == 0 || expect.width > limits.max_dimension ||
      expect.height == 0 || expect.height > limits.max_dimension ||
      expect.frame_count == 0 || expect.frame_count > limits.max_frames) {
    expect.width = expect.height = 16;
    expect.frame_count = 1;
  }

  std::unique_ptr<wt::ByteSource> source;
  if (wt::ByteSource::CreateMemory(file.data(), file.size(), UINT64_MAX,
                                   &source) != WT_OK) {
    return;
  }
  std::unique_ptr<wt::WscReader> reader;
  if (wt::WscReader::Open(std::move(source), expect, limits, &reader) !=
      WT_OK) {
    if (reader != nullptr) __builtin_trap();
    return;
  }
  const wt::WscParams& got = reader->params();
  if (got.width != expect.width || got.height != expect.height ||
      got.has_alpha != expect.has_alpha ||
      got.frame_count != expect.frame_count) {
    __builtin_trap();
  }
  wt::YuvaImage frame;
  if (frame.Allocate(got.width, got.height, got.has_alpha) != WT_OK) return;
  for (int pass = 0; pass < 2; ++pass) {  // The second skips verified CRCs.
    for (uint32_t i = 0; i < got.frame_count; ++i) {
      const wt_status s = reader->ReadFrame(i, &frame);
      if (s != WT_OK && s != WT_ERR_INVALID_DATA) __builtin_trap();
      if (reader->frame(i).duration_ms == 0) __builtin_trap();
    }
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Tighter than the defaults, like fuzz_probe.
  wt_limits limits;
  wt_limits_init_default(&limits);
  limits.max_dimension = 256;
  limits.max_frames = 64;

  std::vector<uint8_t> file(data, data + size);
  Run(file, limits);
  FixCrcs(&file);
  Run(file, limits);
  return 0;
}
