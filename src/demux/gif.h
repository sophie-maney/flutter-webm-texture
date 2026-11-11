// Indexing a GIF file without decoding pixels: canvas size, per-frame rect,
// interlace flag, disposal, transparency index and delay, read sequentially
// with giflib's low-level API (never DGifSlurp). Image data is skipped block
// by block (DGifGetCode), so no LZW stream is expanded here.
//
// Policies (ADR 0023):
//   - Delays are in centiseconds. A delay below 20 ms (0 or 1 cs) plays for
//     100 ms, as browsers do; everything else is the file's value. Durations
//     are therefore always >= 1, and pts is the running sum.
//   - Disposal values above 3 are undefined by the spec and mean "none".
//     The Graphic Control Extension applies to the next image only.
//   - A zero-size logical screen takes the far edge of the first frame
//     (left + width, top + height) per zero dimension. Later frames are
//     clipped to the canvas, so are frames that stick out of a declared one.
//   - has_alpha is true whenever some pixel can end up transparent: a frame
//     declares a transparency index, the first frame doesn't cover the
//     canvas, a frame other than the last uses "restore to background" or
//     the first frame, not being the last, uses "restore to previous", or a
//     frame has no colour map or an LZW code size larger than its colour map
//     needs (max(2, log2(size))), so its pixels can name entries that don't
//     exist (those are transparent, see decode/gif_decoder.h). Conservative:
//     it doesn't look at the pixels.
//   - Limits are enforced like the WebM and WebP demuxers do: max_dimension
//     on the canvas, max_frames on the frame count, max_total_pixels on
//     frames x canvas area, and max_duration_ms on the summed durations.
//     A frame's rect can be up to 65535 x 65535 whatever the canvas, so the
//     sum of the rect areas (the pixels the LZW decoder must produce) is
//     held to max_total_pixels too.
//   - A file that ends before its trailer keeps its complete frames, with
//     `truncated` set; no complete frame is WT_ERR_TRUNCATED. A structural
//     error (bad record type, LZW code size outside 2..8) is
//     WT_ERR_INVALID_DATA, whatever precedes it.
//
// Thread: background thread. No shared state.
#ifndef WT_DEMUX_GIF_H_
#define WT_DEMUX_GIF_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "api/wt_api.h"
#include "demux/source.h"

struct GifFileType;  // giflib's, at global scope.

namespace wt {

struct GifFrame {
  // The image descriptor's rect, unclipped (each 0..65535).
  uint32_t left = 0;
  uint32_t top = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool interlaced = false;
  uint8_t disposal = 0;      // 0 or 1: leave, 2: background, 3: previous.
  int16_t transparent = -1;  // Palette index, or -1 for none.
  uint32_t duration_ms = 1;  // >= 1, after the delay policy.
};

struct GifIndex {
  uint32_t width = 0;   // Canvas size.
  uint32_t height = 0;  //
  bool has_alpha = false;
  bool truncated = false;
  std::vector<GifFrame> frames;  // One per complete frame.
};

// Delay policy for one frame, in centiseconds -> ms. Always >= 1.
uint32_t GifFrameDurationMs(int centiseconds);

// Indexes `bytes`. On failure *out is cleared. Errors: WT_ERR_INVALID_DATA,
// WT_ERR_TRUNCATED (no complete frame), WT_ERR_LIMIT,
// WT_ERR_INVALID_ARGUMENT (a zero limit), WT_ERR_NO_MEMORY.
wt_status DemuxGif(ByteView bytes, const wt_limits& limits, GifIndex* out);

// A giflib decoder reading `bytes` through a bounds-checked in-memory read
// callback, positioned after the screen descriptor and global colour map.
// Shared by the demuxer and GifDecoder. Not thread-safe.
class GifStream {
 public:
  // `bytes` must outlive the stream. Errors: WT_ERR_TRUNCATED (header or
  // global colour map cut short), WT_ERR_INVALID_DATA, WT_ERR_NO_MEMORY.
  static wt_status Open(ByteView bytes, std::unique_ptr<GifStream>* out);
  ~GifStream();
  GifStream(const GifStream&) = delete;
  GifStream& operator=(const GifStream&) = delete;

  GifFileType* file() const { return file_; }

  // Every byte has been handed to giflib. A read error with input left over
  // isn't a truncated file (giflib reports a bad LZW code size as a failed
  // read, for one).
  bool exhausted() const { return reader_.pos >= reader_.bytes.size; }

  // Drops the per-image records giflib's low-level reader appends to
  // GifFileType::SavedImages on every image descriptor (it does so even when
  // not slurping), so a long file costs no memory. Call after each image
  // descriptor, once the frame's facts are copied out.
  void ReleaseSavedImages();

 private:
  struct Reader {
    ByteView bytes;
    size_t pos = 0;
  };
  GifStream() = default;
  static int Read(GifFileType* file, uint8_t* dst, int length);

  Reader reader_;
  GifFileType* file_ = nullptr;
};

// Maps giflib's GifFileType::Error after a failed read: WT_ERR_TRUNCATED for
// a read that ran out of input, WT_ERR_NO_MEMORY, else WT_ERR_INVALID_DATA.
wt_status GifReadStatus(int giflib_error);

}  // namespace wt

#endif  // WT_DEMUX_GIF_H_
