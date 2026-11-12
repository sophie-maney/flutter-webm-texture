// The frame cache file (.wsc): every frame of one animation at one output
// size, as LZ4-compressed YUVA 4:2:0. docs/cache-format.md is the
// specification; the constants and checks here implement version 1 of it.
//
// A WscWriter streams frames into a temporary file while the first playback
// decodes them, then fsyncs it and renames it into place, so a reader only
// ever sees a complete file or none. A WscReader maps a file read-only and
// validates the header and frame table before handing out any frame, and
// each frame's CRC-32 before decompressing it for the first time.
//
// Cache files live in the app's private storage, but they are still parsed as
// untrusted input: a crash, a full disk or a bug can leave any bytes behind,
// and fuzz_wsc feeds the reader arbitrary files.
//
// Thread: background thread (file I/O, compression). Neither class is
// thread-safe; one instance per animation.
#ifndef WT_CACHE_WSC_H_
#define WT_CACHE_WSC_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "api/wt_api.h"
#include "convert/yuva.h"
#include "demux/source.h"

namespace wt {

constexpr uint32_t kWscVersion = 1;
constexpr size_t kWscHeaderSize = 64;
constexpr size_t kWscEntrySize = 32;

// What a cache file describes. A reader only accepts a file whose fields all
// match the caller's expectation; a writer stores them in the header.
struct WscParams {
  uint32_t width = 0;   // Output size, 1..YuvaImage::kMaxDimension.
  uint32_t height = 0;  //
  bool has_alpha = false;
  uint32_t frame_count = 0;  // >= 1.
  // Fingerprint of the encoded source the frames were decoded from, so a
  // cache never outlives a change of content behind the same cache key.
  uint64_t source_size = 0;
  uint32_t source_crc = 0;  // Crc32 of the whole source.
};

// One frame-table entry.
struct WscFrame {
  uint64_t offset = 0;  // From the start of the file.
  uint32_t size = 0;    // Compressed bytes.
  uint32_t crc = 0;     // Crc32 of the compressed bytes.
  uint32_t duration_ms = 0;
  int64_t pts_ms = 0;
};

// Bytes of one uncompressed frame: Y, U, V and, with alpha, A planes, packed
// without padding. Dimensions must be in 1..YuvaImage::kMaxDimension.
size_t WscFrameBytes(uint32_t width, uint32_t height, bool has_alpha);

// The largest file a valid cache for params can be: header, table, and every
// frame at LZ4's worst-case size. Readers refuse to map anything larger.
uint64_t WscMaxFileSize(const WscParams& params);

// The smallest file a cache for params can be: header, table, and every frame
// at LZ4's best ratio (each byte of an LZ4 block decodes to at most about
// 255 bytes, so a frame takes at least frame_bytes / 256). No content
// compresses below it, so refusing a write for it can't refuse anything that
// would fit.
uint64_t WscMinFileSize(const WscParams& params);

class WscReader {
 public:
  ~WscReader();
  WscReader(const WscReader&) = delete;
  WscReader& operator=(const WscReader&) = delete;

  // Validates file (a whole cache file) and takes ownership of it. Checks the
  // magic, version, header and table CRCs, that every field matches expect,
  // that width, height and frame_count are within limits, and that the
  // frames tile the rest of the file exactly. Per-frame CRCs are checked by
  // ReadFrame. Errors: WT_ERR_INVALID_DATA (damaged, partial or for other
  // content), WT_ERR_UNSUPPORTED (another format version),
  // WT_ERR_INVALID_ARGUMENT (bad expect), WT_ERR_NO_MEMORY. Callers delete
  // a file that fails.
  static wt_status Open(std::unique_ptr<ByteSource> file,
                        const WscParams& expect, const wt_limits& limits,
                        std::unique_ptr<WscReader>* out);

  const WscParams& params() const { return params_; }
  YuvColor color() const { return color_; }
  const WscFrame& frame(uint32_t index) const { return frames_[index]; }

  // Decompresses frame index (< frame_count) into dst, which must be
  // width x height with an alpha plane iff has_alpha, and sets dst's colour
  // signalling. The first read of each frame checks its CRC. Returns
  // WT_ERR_INVALID_DATA for a CRC mismatch or bad LZ4 data, leaving dst
  // partly written; WT_ERR_INVALID_ARGUMENT for a bad index or dst.
  wt_status ReadFrame(uint32_t index, YuvaImage* dst);

 private:
  WscReader() = default;

  std::unique_ptr<ByteSource> file_;
  WscParams params_;
  YuvColor color_;
  size_t frame_bytes_ = 0;
  // Copied out of the mapping at Open, so the entries ReadFrame trusts are
  // the ones that were validated.
  std::unique_ptr<WscFrame[]> frames_;
  std::unique_ptr<bool[]> verified_;  // Frame i's CRC has matched.
};

class WscWriter {
 public:
  // Abandons an unfinished file: closes and unlinks the temporary file.
  ~WscWriter();
  WscWriter(const WscWriter&) = delete;
  WscWriter& operator=(const WscWriter&) = delete;

  // Starts a cache file for params (with frames in the colour signalling
  // color) that Finish will rename to path, and that may be at most
  // max_file_size bytes (T6.1, S-04, ADR 0026). Creates a uniquely named
  // temporary file next to it, so animations writing the same path don't
  // collide; the directory must exist. Errors: WT_ERR_INVALID_ARGUMENT,
  // WT_ERR_LIMIT when even WscMinFileSize(params) is over max_file_size
  // (nothing is created), WT_ERR_IO, WT_ERR_NO_MEMORY.
  static wt_status Create(const char* path, const WscParams& params,
                          YuvColor color, uint64_t max_file_size,
                          std::unique_ptr<WscWriter>* out);

  uint32_t frames_written() const { return written_; }
  bool full() const { return written_ == params_.frame_count; }
  uint64_t max_file_size() const { return max_file_size_; }

  // Compresses and appends the next frame, which must be width x height with
  // alpha iff has_alpha. duration_ms must be >= 1. WT_ERR_INVALID_ARGUMENT
  // for a frame that doesn't fit (or one too many); WT_ERR_LIMIT when the
  // file would grow past max_file_size, and WT_ERR_IO when the write fails:
  // both abandon the file (the temporary is deleted), after which the writer
  // only fails.
  wt_status Append(const YuvaView& frame, uint32_t duration_ms, int64_t pts_ms);

  // Once every frame is appended: writes the table and header, fsyncs the
  // file, renames it over path and fsyncs the directory. Replacing an
  // existing file is atomic; readers that mapped the old one keep it.
  // WT_ERR_INVALID_ARGUMENT before the last frame, WT_ERR_IO on failure
  // (nothing is left behind).
  wt_status Finish();

 private:
  WscWriter() = default;

  // Writes size bytes at offset, retrying short writes and EINTR.
  bool WriteAt(const uint8_t* data, size_t size, uint64_t offset);
  void Abandon();

  std::string path_;
  std::string temp_path_;
  int fd_ = -1;  // -1 once finished or failed.
  WscParams params_;
  YuvColor color_;
  size_t frame_bytes_ = 0;
  uint64_t max_file_size_ = 0;
  uint32_t written_ = 0;
  uint64_t next_offset_ = 0;           // Where the next frame's bytes go.
  std::unique_ptr<uint8_t[]> table_;   // frame_count encoded entries.
  std::unique_ptr<uint8_t[]> raw_;     // One packed frame.
  std::unique_ptr<uint8_t[]> packed_;  // One compressed frame.
  size_t packed_capacity_ = 0;
};

}  // namespace wt

#endif  // WT_CACHE_WSC_H_
