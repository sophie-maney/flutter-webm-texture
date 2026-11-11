#include "demux/gif.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "convert/yuva.h"
#include "gif_lib.h"

namespace wt {

namespace {

constexpr uint32_t kMinDelayMs = 20;  // Below this, 100 ms.
constexpr uint32_t kDefaultDelayMs = 100;

// Checks what the canvas alone decides. Called as early as possible so a
// hostile file costs little.
wt_status CheckCanvas(uint32_t width, uint32_t height,
                      const wt_limits& limits) {
  if (width == 0 || height == 0) return WT_ERR_INVALID_DATA;
  if (width > limits.max_dimension || height > limits.max_dimension ||
      width > YuvaImage::kMaxDimension || height > YuvaImage::kMaxDimension) {
    return WT_ERR_LIMIT;
  }
  return WT_OK;
}

// The effective palette size of the image just read: its local colour map, or
// else the global one, or 0.
uint32_t PaletteSize(const GifFileType* file) {
  const ColorMapObject* map =
      file->Image.ColorMap != nullptr ? file->Image.ColorMap : file->SColorMap;
  return map != nullptr && map->ColorCount > 0
             ? static_cast<uint32_t>(map->ColorCount)
             : 0;
}

// After a failed giflib read: true when the input simply ended (the caller
// stops indexing and keeps what it has), else false with *s the error.
bool EndedEarly(const GifStream& stream, wt_status* s) {
  *s = GifReadStatus(stream.file()->Error);
  if (*s == WT_ERR_TRUNCATED && !stream.exhausted()) *s = WT_ERR_INVALID_DATA;
  return *s == WT_ERR_TRUNCATED;
}

// log2 of a colour map size (giflib's are powers of two, 2..256).
uint32_t PaletteBits(uint32_t colors) {
  uint32_t bits = 0;
  while ((1u << bits) < colors) ++bits;
  return bits;
}

}  // namespace

uint32_t GifFrameDurationMs(int centiseconds) {
  if (centiseconds < 0) return kDefaultDelayMs;
  const uint64_t ms = uint64_t{static_cast<uint32_t>(centiseconds)} * 10;
  if (ms < kMinDelayMs) return kDefaultDelayMs;
  return static_cast<uint32_t>(std::min<uint64_t>(ms, UINT32_MAX));
}

wt_status GifReadStatus(int error) {
  switch (error) {
    case D_GIF_ERR_READ_FAILED:
    case D_GIF_ERR_EOF_TOO_SOON:
      return WT_ERR_TRUNCATED;
    case D_GIF_ERR_NOT_ENOUGH_MEM:
      return WT_ERR_NO_MEMORY;
    default:
      return WT_ERR_INVALID_DATA;
  }
}

int GifStream::Read(GifFileType* file, uint8_t* dst, int length) {
  Reader* r = static_cast<Reader*>(file->UserData);
  if (length <= 0 || r->pos >= r->bytes.size) return 0;
  const size_t n =
      std::min(static_cast<size_t>(length), r->bytes.size - r->pos);
  std::memcpy(dst, r->bytes.data + r->pos, n);
  r->pos += n;
  return static_cast<int>(n);
}

wt_status GifStream::Open(ByteView bytes, std::unique_ptr<GifStream>* out) {
  out->reset();
  if (bytes.data == nullptr && bytes.size != 0) return WT_ERR_INVALID_ARGUMENT;
  std::unique_ptr<GifStream> s(new (std::nothrow) GifStream);
  if (s == nullptr) return WT_ERR_NO_MEMORY;
  s->reader_.bytes = bytes;
  int error = D_GIF_SUCCEEDED;
  s->file_ = DGifOpen(&s->reader_, &GifStream::Read, &error);
  if (s->file_ == nullptr) {
    // giflib says "no screen descriptor" for a header that ends early, and
    // "read failed" for a colour map that does.
    if (s->exhausted() &&
        (error == D_GIF_ERR_NO_SCRN_DSCR || error == D_GIF_ERR_READ_FAILED)) {
      return WT_ERR_TRUNCATED;
    }
    const wt_status status = GifReadStatus(error);
    return status == WT_ERR_TRUNCATED ? WT_ERR_INVALID_DATA : status;
  }
  *out = std::move(s);
  return WT_OK;
}

GifStream::~GifStream() {
  if (file_ != nullptr) DGifCloseFile(file_, nullptr);
}

void GifStream::ReleaseSavedImages() {
  GifFreeSavedImages(file_);
  file_->ImageCount = 0;
}

wt_status DemuxGif(ByteView bytes, const wt_limits& limits, GifIndex* out) {
  *out = GifIndex();
  if ((bytes.data == nullptr && bytes.size != 0) || limits.max_frames == 0 ||
      limits.max_dimension == 0 || limits.max_total_pixels == 0 ||
      limits.max_duration_ms == 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  auto fail = [out](wt_status s) {
    *out = GifIndex();
    return s;
  };

  std::unique_ptr<GifStream> stream;
  wt_status s = GifStream::Open(bytes, &stream);
  if (s != WT_OK) return fail(s);
  GifFileType* file = stream->file();

  // A zero dimension is taken from the first frame once it is known.
  uint32_t width = file->SWidth > 0 ? static_cast<uint32_t>(file->SWidth) : 0;
  uint32_t height =
      file->SHeight > 0 ? static_cast<uint32_t>(file->SHeight) : 0;
  const bool zero_width = width == 0, zero_height = height == 0;
  if (!zero_width && !zero_height) {
    s = CheckCanvas(width, height, limits);
    if (s != WT_OK) return fail(s);
  }

  GraphicsControlBlock gcb;
  gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
  gcb.UserInputFlag = false;
  gcb.DelayTime = 0;
  gcb.TransparentColor = NO_TRANSPARENT_COLOR;
  const GraphicsControlBlock no_gcb = gcb;

  bool terminated = false;
  bool has_alpha = false;
  bool covers_canvas = true;  // The first frame, clipped, fills the canvas.
  bool first_restores_previous = false;
  bool any_restores_background = false;  // Of the frames before the last.
  uint64_t total_ms = 0;
  uint64_t rect_pixels = 0;

  for (;;) {
    GifRecordType type = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(file, &type) != GIF_OK) {
      if (!EndedEarly(*stream, &s)) return fail(s);
      break;  // Ended without a trailer.
    }
    if (type == TERMINATE_RECORD_TYPE) {
      terminated = true;
      break;
    }
    if (type == EXTENSION_RECORD_TYPE) {
      int code = 0;
      GifByteType* block = nullptr;
      if (DGifGetExtension(file, &code, &block) != GIF_OK) {
        if (!EndedEarly(*stream, &s)) return fail(s);
        break;
      }
      bool cut = false;
      bool first = true;
      while (block != nullptr) {
        // Only the first sub-block of a Graphic Control Extension counts; one
        // of the wrong length is ignored, as the spec has it.
        if (first && code == GRAPHICS_EXT_FUNC_CODE) {
          GraphicsControlBlock parsed;
          if (DGifExtensionToGCB(block[0], block + 1, &parsed) == GIF_OK) {
            gcb = parsed;
          }
        }
        first = false;
        if (DGifGetExtensionNext(file, &block) != GIF_OK) {
          if (!EndedEarly(*stream, &s)) return fail(s);
          cut = true;
          break;
        }
      }
      if (cut) break;
      continue;
    }

    // An image. The check comes before DGifGetImageDesc so that giflib never
    // holds more than max_frames records for a hostile stream.
    if (out->frames.size() >= limits.max_frames) return fail(WT_ERR_LIMIT);
    if (DGifGetImageDesc(file) != GIF_OK) {
      if (!EndedEarly(*stream, &s)) return fail(s);
      break;
    }
    GifFrame frame;
    frame.left = static_cast<uint32_t>(file->Image.Left);
    frame.top = static_cast<uint32_t>(file->Image.Top);
    frame.width = static_cast<uint32_t>(file->Image.Width);
    frame.height = static_cast<uint32_t>(file->Image.Height);
    frame.interlaced = file->Image.Interlace;
    frame.disposal = gcb.DisposalMode >= 0 && gcb.DisposalMode <= 3
                         ? static_cast<uint8_t>(gcb.DisposalMode)
                         : 0;
    frame.transparent = gcb.TransparentColor >= 0
                            ? static_cast<int16_t>(gcb.TransparentColor)
                            : int16_t{-1};
    frame.duration_ms = GifFrameDurationMs(gcb.DelayTime);
    gcb = no_gcb;  // A GCE covers the next image only.
    const uint32_t palette = PaletteSize(file);
    stream->ReleaseSavedImages();

    if (out->frames.empty()) {
      if (zero_width) width = frame.left + frame.width;
      if (zero_height) height = frame.top + frame.height;
      s = CheckCanvas(width, height, limits);
      if (s != WT_OK) return fail(s);
      covers_canvas = frame.left == 0 && frame.top == 0 &&
                      frame.width >= width && frame.height >= height;
    }

    // Skip the image data without expanding it. The first block also tells
    // the LZW code size, which bounds the indices the pixels can use.
    int code_size = 0;
    GifByteType* block = nullptr;
    if (DGifGetCode(file, &code_size, &block) != GIF_OK) {
      if (!EndedEarly(*stream, &s)) return fail(s);
      break;
    }
    if (code_size < 2 || code_size > 8) return fail(WT_ERR_INVALID_DATA);
    bool cut = false;
    while (block != nullptr) {
      if (DGifGetCodeNext(file, &block) != GIF_OK) {
        if (!EndedEarly(*stream, &s)) return fail(s);
        cut = true;
        break;
      }
    }
    if (cut) break;

    // The frame is complete: account for it.
    total_ms += frame.duration_ms;
    if (total_ms > limits.max_duration_ms) return fail(WT_ERR_LIMIT);
    rect_pixels += uint64_t{frame.width} * frame.height;
    if (rect_pixels > limits.max_total_pixels) return fail(WT_ERR_LIMIT);
    if (uint64_t{width} * height >
        limits.max_total_pixels / (out->frames.size() + 1)) {
      return fail(WT_ERR_LIMIT);
    }
    // An index the palette doesn't have draws nothing, i.e. is transparent.
    // A conforming file uses a code size of max(2, log2(palette size)); a
    // larger one, or no palette at all, can name indices past the map.
    if (frame.transparent >= 0 || palette == 0 ||
        static_cast<uint32_t>(code_size) > std::max(2u, PaletteBits(palette))) {
      has_alpha = true;
    }
    if (out->frames.empty() && frame.disposal == 3) {
      first_restores_previous = true;
    }
    out->frames.push_back(frame);
  }

  if (out->frames.empty()) {
    return fail(terminated ? WT_ERR_INVALID_DATA : WT_ERR_TRUNCATED);
  }
  const size_t last = out->frames.size() - 1;
  for (size_t i = 0; i < last; ++i) {
    if (out->frames[i].disposal == 2) any_restores_background = true;
  }
  out->width = width;
  out->height = height;
  out->truncated = !terminated;
  out->has_alpha = has_alpha || !covers_canvas || any_restores_background ||
                   (first_restores_previous && last > 0);
  return WT_OK;
}

}  // namespace wt
