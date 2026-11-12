#include "decode/gif_decoder.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "gif_lib.h"

namespace wt {

namespace {

// The canvas row that the r-th row of an interlaced image of `height` rows
// lands on: pass 1 is every 8th row from 0, pass 2 every 8th from 4, pass 3
// every 4th from 2, pass 4 every 2nd from 1.
uint32_t InterlacedRow(uint32_t r, uint32_t height) {
  const uint32_t n1 = (height + 7) / 8, n2 = (height + 3) / 8,
                 n3 = (height + 1) / 4;
  if (r < n1) return r * 8;
  r -= n1;
  if (r < n2) return 4 + r * 8;
  r -= n2;
  if (r < n3) return 2 + r * 4;
  return 1 + (r - n3) * 2;
}

wt_status DecodeStatus(const GifFileType* file) {
  return file->Error == D_GIF_ERR_NOT_ENOUGH_MEM ? WT_ERR_NO_MEMORY
                                                 : WT_ERR_DECODE;
}

}  // namespace

GifDecoder::Clip GifDecoder::ClipOf(const GifFrame& f) const {
  Clip c;
  c.x0 = std::min(f.left, width_);
  c.y0 = std::min(f.top, height_);
  c.x1 = std::min(f.left + f.width, width_);  // Each <= 65535 + 65535.
  c.y1 = std::min(f.top + f.height, height_);
  return c;
}

wt_status GifDecoder::Create(ByteView bytes, const GifIndex& index,
                             std::unique_ptr<GifDecoder>* out) {
  out->reset();
  if (index.frames.empty() || index.width == 0 || index.height == 0) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  std::unique_ptr<GifDecoder> d(new (std::nothrow) GifDecoder);
  if (d == nullptr) return WT_ERR_NO_MEMORY;
  d->bytes_ = bytes;
  d->width_ = index.width;
  d->height_ = index.height;
  d->frames_ = index.frames;
  for (const GifFrame& f : d->frames_) {
    d->line_capacity_ = std::max<size_t>(d->line_capacity_, f.width);
    if (f.disposal == 3) {
      d->saved_capacity_ = std::max(d->saved_capacity_, d->ClipOf(f).bytes());
    }
  }
  const size_t canvas_bytes = size_t{d->width_} * d->height_ * 4;
  d->canvas_.reset(new (std::nothrow) uint8_t[canvas_bytes]());
  d->line_.reset(new (std::nothrow) uint8_t[d->line_capacity_ + 1]);
  if (d->saved_capacity_ != 0) {
    d->saved_.reset(new (std::nothrow) uint8_t[d->saved_capacity_]);
  }
  if (d->canvas_ == nullptr || d->line_ == nullptr ||
      (d->saved_capacity_ != 0 && d->saved_ == nullptr)) {
    return WT_ERR_NO_MEMORY;
  }
  const wt_status s = GifStream::Open(bytes, &d->stream_);
  if (s != WT_OK) return s == WT_ERR_TRUNCATED ? WT_ERR_INVALID_DATA : s;
  const GifFileType* file = d->stream_->file();
  // A zero-size screen was resolved from the first frame by the demuxer.
  if ((file->SWidth != 0 && static_cast<uint32_t>(file->SWidth) != d->width_) ||
      (file->SHeight != 0 &&
       static_cast<uint32_t>(file->SHeight) != d->height_)) {
    return WT_ERR_INVALID_DATA;
  }
  *out = std::move(d);
  return WT_OK;
}

GifDecoder::~GifDecoder() = default;

wt_status GifDecoder::Reset() {
  position_ = 0;
  pending_disposal_ = 0;
  pending_clip_ = Clip();
  std::memset(canvas_.get(), 0, size_t{width_} * height_ * 4);
  stream_.reset();
  failed_ = true;  // Until the stream is back.
  const wt_status s = GifStream::Open(bytes_, &stream_);
  if (s != WT_OK) return s == WT_ERR_NO_MEMORY ? s : WT_ERR_INTERNAL;
  failed_ = false;
  return WT_OK;
}

void GifDecoder::ApplyDisposal() {
  const Clip c = pending_clip_;
  const uint8_t disposal = pending_disposal_;
  pending_disposal_ = 0;
  if (disposal < 2 || c.x1 <= c.x0 || c.y1 <= c.y0) return;
  const size_t row_bytes = size_t{c.x1 - c.x0} * 4;
  const uint8_t* src = saved_.get();
  for (uint32_t y = c.y0; y < c.y1; ++y) {
    uint8_t* dst = canvas_.get() + (size_t{y} * width_ + c.x0) * 4;
    if (disposal == 2) {
      std::memset(dst, 0, row_bytes);
    } else {
      std::memcpy(dst, src, row_bytes);
      src += row_bytes;
    }
  }
}

// Reads records up to and including the image descriptor of frame `f`.
wt_status GifDecoder::ReachImage(const GifFrame& f) {
  GifFileType* file = stream_->file();
  for (;;) {
    GifRecordType type = UNDEFINED_RECORD_TYPE;
    if (DGifGetRecordType(file, &type) != GIF_OK) return DecodeStatus(file);
    if (type == EXTENSION_RECORD_TYPE) {
      int code = 0;
      GifByteType* block = nullptr;
      if (DGifGetExtension(file, &code, &block) != GIF_OK) {
        return DecodeStatus(file);
      }
      while (block != nullptr) {
        if (DGifGetExtensionNext(file, &block) != GIF_OK) {
          return DecodeStatus(file);
        }
      }
      continue;
    }
    if (type != IMAGE_DESC_RECORD_TYPE) return WT_ERR_DECODE;
    if (DGifGetImageDesc(file) != GIF_OK) return DecodeStatus(file);
    break;
  }
  const GifImageDesc& d = file->Image;
  const bool same = d.Left >= 0 && static_cast<uint32_t>(d.Left) == f.left &&
                    d.Top >= 0 && static_cast<uint32_t>(d.Top) == f.top &&
                    d.Width >= 0 && static_cast<uint32_t>(d.Width) == f.width &&
                    d.Height >= 0 &&
                    static_cast<uint32_t>(d.Height) == f.height &&
                    d.Interlace == f.interlaced;
  stream_->ReleaseSavedImages();
  return same ? WT_OK : WT_ERR_DECODE;
}

wt_status GifDecoder::DrawImage(const GifFrame& f, const Clip& clip) {
  GifFileType* file = stream_->file();
  const ColorMapObject* map =
      file->Image.ColorMap != nullptr ? file->Image.ColorMap : file->SColorMap;
  // Palette entries as RGBA pixels; indices past the map draw nothing.
  const uint32_t colors =
      map != nullptr && map->ColorCount > 0
          ? std::min<uint32_t>(static_cast<uint32_t>(map->ColorCount), 256)
          : 0;
  uint8_t palette[256][4];
  for (uint32_t i = 0; i < colors; ++i) {
    palette[i][0] = map->Colors[i].Red;
    palette[i][1] = map->Colors[i].Green;
    palette[i][2] = map->Colors[i].Blue;
    palette[i][3] = 255;
  }
  const int transparent = f.transparent;
  if (f.width == 0 || f.height == 0) {
    // No rows to read, so nothing flushes the (empty) data blocks.
    GifByteType* block = nullptr;
    int code_size = 0;
    if (DGifGetCode(file, &code_size, &block) != GIF_OK) {
      return DecodeStatus(file);
    }
    while (block != nullptr) {
      if (DGifGetCodeNext(file, &block) != GIF_OK) return DecodeStatus(file);
    }
    return WT_OK;
  }
  const uint32_t visible_w = clip.x1 - clip.x0;
  for (uint32_t r = 0; r < f.height; ++r) {
    if (DGifGetLine(file, line_.get(), static_cast<int>(f.width)) != GIF_OK) {
      return DecodeStatus(file);
    }
    const uint32_t y = f.top + (f.interlaced ? InterlacedRow(r, f.height) : r);
    if (y >= height_) continue;  // Below the canvas: consumed, dropped.
    uint8_t* dst = canvas_.get() + (size_t{y} * width_ + clip.x0) * 4;
    for (uint32_t x = 0; x < visible_w; ++x, dst += 4) {
      const uint8_t index = line_[x];
      if (index == transparent || index >= colors) continue;
      std::memcpy(dst, palette[index], 4);
    }
  }
  return WT_OK;
}

wt_status GifDecoder::Next(RgbaView* out) {
  *out = RgbaView();
  if (position_ >= frames_.size()) return WT_END;
  if (failed_) return WT_ERR_DECODE;
  const GifFrame& f = frames_[position_];
  const Clip clip = ClipOf(f);

  ApplyDisposal();
  failed_ = true;  // Until the frame is drawn.
  wt_status s = ReachImage(f);
  if (s != WT_OK) return s;
  if (f.disposal == 3 && clip.x1 > clip.x0 && clip.y1 > clip.y0) {
    // The rect as it is before this frame draws on it.
    const size_t row_bytes = size_t{clip.x1 - clip.x0} * 4;
    uint8_t* dst = saved_.get();
    for (uint32_t y = clip.y0; y < clip.y1; ++y, dst += row_bytes) {
      std::memcpy(dst, canvas_.get() + (size_t{y} * width_ + clip.x0) * 4,
                  row_bytes);
    }
  }
  s = DrawImage(f, clip);
  if (s != WT_OK) return s;
  failed_ = false;
  pending_disposal_ = f.disposal;
  pending_clip_ = clip;
  ++position_;
  out->data = canvas_.get();
  out->stride = size_t{width_} * 4;
  out->width = width_;
  out->height = height_;
  return WT_OK;
}

}  // namespace wt
