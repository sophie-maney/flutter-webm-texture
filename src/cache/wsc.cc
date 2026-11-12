#include "cache/wsc.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "cache/crc32.h"
#include "lz4.h"

namespace wt {

namespace {

// docs/cache-format.md, "Header". Every multi-byte field is little-endian.
constexpr uint8_t kMagic[8] = {'W', 'T', 'S', 'C', '\r', '\n', 0x1A, '\n'};
constexpr size_t kOffVersion = 8;
constexpr size_t kOffHeaderSize = 12;
constexpr size_t kOffWidth = 16;
constexpr size_t kOffHeight = 20;
constexpr size_t kOffFrameCount = 24;
constexpr size_t kOffFlags = 28;
constexpr size_t kOffMatrix = 32;
constexpr size_t kOffFullRange = 33;
constexpr size_t kOffReserved = 34;  // u16, zero.
constexpr size_t kOffSourceCrc = 36;
constexpr size_t kOffSourceSize = 40;
constexpr size_t kOffFileSize = 48;
constexpr size_t kOffTableCrc = 56;
constexpr size_t kOffHeaderCrc = 60;

constexpr uint32_t kFlagAlpha = 1u << 0;

// "Frame table" entries.
constexpr size_t kEntOffset = 0;
constexpr size_t kEntPts = 8;
constexpr size_t kEntSize = 16;
constexpr size_t kEntCrc = 20;
constexpr size_t kEntDuration = 24;
constexpr size_t kEntReserved = 28;  // u32, zero.

static_assert(kOffHeaderCrc + 4 == kWscHeaderSize, "header layout");
static_assert(kEntReserved + 4 == kWscEntrySize, "entry layout");

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

void Put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

bool DimensionsValid(uint32_t width, uint32_t height) {
  return width >= 1 && height >= 1 && width <= YuvaImage::kMaxDimension &&
         height <= YuvaImage::kMaxDimension;
}

// The params a file may describe at all, before comparing with anything.
bool ParamsValid(const WscParams& p) {
  return DimensionsValid(p.width, p.height) && p.frame_count >= 1 &&
         WscFrameBytes(p.width, p.height, p.has_alpha) <=
             static_cast<size_t>(LZ4_MAX_INPUT_SIZE);
}

uint32_t CompressBound(size_t frame_bytes) {
  return static_cast<uint32_t>(
      LZ4_compressBound(static_cast<int>(frame_bytes)));
}

uint64_t DataStart(uint32_t frame_count) {
  return kWscHeaderSize + uint64_t{frame_count} * kWscEntrySize;
}

uint8_t EncodeMatrix(YuvMatrix m) {
  switch (m) {
    case YuvMatrix::kBt601:
      return 0;
    case YuvMatrix::kBt709:
      return 1;
    case YuvMatrix::kBt2020:
      return 2;
  }
  return 0;
}

bool DecodeMatrix(uint8_t value, YuvMatrix* out) {
  switch (value) {
    case 0:
      *out = YuvMatrix::kBt601;
      return true;
    case 1:
      *out = YuvMatrix::kBt709;
      return true;
    case 2:
      *out = YuvMatrix::kBt2020;
      return true;
    default:
      return false;
  }
}

// A per-process counter keeps temporary names unique between threads; the
// pid keeps them unique between processes sharing the directory.
std::atomic<uint32_t> g_temp_counter{0};

}  // namespace

size_t WscFrameBytes(uint32_t width, uint32_t height, bool has_alpha) {
  // Same arithmetic as YuvaImage::Allocate, whose buffer this mirrors.
  const size_t luma = size_t{width} * height;
  const size_t chroma = size_t{ChromaSize(width)} * ChromaSize(height);
  return luma + 2 * chroma + (has_alpha ? luma : 0);
}

uint64_t WscMaxFileSize(const WscParams& params) {
  if (!ParamsValid(params)) return 0;
  const size_t frame_bytes =
      WscFrameBytes(params.width, params.height, params.has_alpha);
  // frame_count < 2^32 and the bound < 2^31, so this can't overflow.
  return DataStart(params.frame_count) +
         uint64_t{params.frame_count} * CompressBound(frame_bytes);
}

uint64_t WscMinFileSize(const WscParams& params) {
  if (!ParamsValid(params)) return 0;
  const size_t frame_bytes =
      WscFrameBytes(params.width, params.height, params.has_alpha);
  // Every frame is at least 1 byte (the reader requires it).
  const uint64_t min_frame = std::max<uint64_t>(1, frame_bytes / 256);
  return DataStart(params.frame_count) +
         uint64_t{params.frame_count} * min_frame;
}

// --- Reader -----------------------------------------------------------------

WscReader::~WscReader() = default;

wt_status WscReader::Open(std::unique_ptr<ByteSource> file,
                          const WscParams& expect, const wt_limits& limits,
                          std::unique_ptr<WscReader>* out) {
  out->reset();
  if (file == nullptr || !ParamsValid(expect)) return WT_ERR_INVALID_ARGUMENT;
  const ByteView bytes = file->whole();
  if (bytes.size < kWscHeaderSize) return WT_ERR_INVALID_DATA;
  const uint8_t* h = bytes.data;

  if (std::memcmp(h, kMagic, sizeof(kMagic)) != 0) return WT_ERR_INVALID_DATA;
  if (Get32(h + kOffHeaderCrc) != Crc32(0, h, kOffHeaderCrc)) {
    return WT_ERR_INVALID_DATA;
  }
  // Checked after the CRC so a damaged version field reads as damage.
  if (Get32(h + kOffVersion) != kWscVersion) return WT_ERR_UNSUPPORTED;
  if (Get32(h + kOffHeaderSize) != kWscHeaderSize) return WT_ERR_INVALID_DATA;

  WscParams params;
  params.width = Get32(h + kOffWidth);
  params.height = Get32(h + kOffHeight);
  params.frame_count = Get32(h + kOffFrameCount);
  const uint32_t flags = Get32(h + kOffFlags);
  params.has_alpha = (flags & kFlagAlpha) != 0;
  params.source_crc = Get32(h + kOffSourceCrc);
  params.source_size = Get64(h + kOffSourceSize);
  YuvColor color;
  if ((flags & ~kFlagAlpha) != 0 ||
      !DecodeMatrix(h[kOffMatrix], &color.matrix) || h[kOffFullRange] > 1 ||
      h[kOffReserved] != 0 || h[kOffReserved + 1] != 0) {
    return WT_ERR_INVALID_DATA;
  }
  color.full_range = h[kOffFullRange] == 1;

  // Limits first, so the comparisons below never see a size we'd refuse.
  if (params.width > limits.max_dimension ||
      params.height > limits.max_dimension ||
      params.frame_count > limits.max_frames || !ParamsValid(params)) {
    return WT_ERR_INVALID_DATA;
  }
  if (params.width != expect.width || params.height != expect.height ||
      params.has_alpha != expect.has_alpha ||
      params.frame_count != expect.frame_count ||
      params.source_size != expect.source_size ||
      params.source_crc != expect.source_crc) {
    return WT_ERR_INVALID_DATA;
  }

  // A partial file (a torn write, a truncation) fails here.
  const uint64_t file_size = Get64(h + kOffFileSize);
  const uint64_t data_start = DataStart(params.frame_count);
  if (file_size != bytes.size || file_size < data_start ||
      file_size > WscMaxFileSize(params)) {
    return WT_ERR_INVALID_DATA;
  }
  const uint8_t* table = bytes.data + kWscHeaderSize;
  const size_t table_size = size_t{params.frame_count} * kWscEntrySize;
  if (Get32(h + kOffTableCrc) != Crc32(0, table, table_size)) {
    return WT_ERR_INVALID_DATA;
  }

  std::unique_ptr<WscReader> reader(new (std::nothrow) WscReader);
  if (reader == nullptr) return WT_ERR_NO_MEMORY;
  reader->frames_.reset(new (std::nothrow) WscFrame[params.frame_count]);
  reader->verified_.reset(new (std::nothrow) bool[params.frame_count]());
  if (reader->frames_ == nullptr || reader->verified_ == nullptr) {
    return WT_ERR_NO_MEMORY;
  }
  reader->frame_bytes_ =
      WscFrameBytes(params.width, params.height, params.has_alpha);
  const uint32_t bound = CompressBound(reader->frame_bytes_);

  // Frames follow the table back to back, in order, up to the end of file.
  uint64_t next = data_start;
  for (uint32_t i = 0; i < params.frame_count; ++i) {
    const uint8_t* e = table + size_t{i} * kWscEntrySize;
    WscFrame& f = reader->frames_[i];
    f.offset = Get64(e + kEntOffset);
    f.pts_ms = static_cast<int64_t>(Get64(e + kEntPts));
    f.size = Get32(e + kEntSize);
    f.crc = Get32(e + kEntCrc);
    f.duration_ms = Get32(e + kEntDuration);
    if (f.offset != next || f.size == 0 || f.size > bound ||
        f.size > file_size - next || f.duration_ms == 0 ||
        Get32(e + kEntReserved) != 0) {
      return WT_ERR_INVALID_DATA;
    }
    next += f.size;
  }
  if (next != file_size) return WT_ERR_INVALID_DATA;

  reader->file_ = std::move(file);
  reader->params_ = params;
  reader->color_ = color;
  *out = std::move(reader);
  return WT_OK;
}

wt_status WscReader::ReadFrame(uint32_t index, YuvaImage* dst) {
  if (index >= params_.frame_count || dst == nullptr ||
      dst->width() != params_.width || dst->height() != params_.height ||
      dst->has_alpha() != params_.has_alpha ||
      dst->data_size() != frame_bytes_) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const WscFrame& f = frames_[index];
  ByteView packed;
  if (!file_->Slice(f.offset, f.size, &packed)) return WT_ERR_INTERNAL;
  // The mapping never changes (writers rename a new file into place), so one
  // successful check per frame is enough.
  if (!verified_[index]) {
    if (Crc32(0, packed.data, packed.size) != f.crc) return WT_ERR_INVALID_DATA;
    verified_[index] = true;
  }
  const int n = LZ4_decompress_safe(reinterpret_cast<const char*>(packed.data),
                                    reinterpret_cast<char*>(dst->data()),
                                    static_cast<int>(packed.size),
                                    static_cast<int>(frame_bytes_));
  if (n < 0 || static_cast<size_t>(n) != frame_bytes_) {
    return WT_ERR_INVALID_DATA;
  }
  dst->set_color(color_);
  return WT_OK;
}

// --- Writer -----------------------------------------------------------------

WscWriter::~WscWriter() { Abandon(); }

void WscWriter::Abandon() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  if (!temp_path_.empty()) {
    unlink(temp_path_.c_str());
    temp_path_.clear();
  }
}

wt_status WscWriter::Create(const char* path, const WscParams& params,
                            YuvColor color, uint64_t max_file_size,
                            std::unique_ptr<WscWriter>* out) {
  out->reset();
  if (path == nullptr || path[0] == '\0' || !ParamsValid(params)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  if (WscMinFileSize(params) > max_file_size) return WT_ERR_LIMIT;
  std::unique_ptr<WscWriter> w(new (std::nothrow) WscWriter);
  if (w == nullptr) return WT_ERR_NO_MEMORY;
  w->params_ = params;
  w->color_ = color;
  w->max_file_size_ = max_file_size;
  w->frame_bytes_ =
      WscFrameBytes(params.width, params.height, params.has_alpha);
  w->packed_capacity_ = CompressBound(w->frame_bytes_);
  const size_t table_size = size_t{params.frame_count} * kWscEntrySize;
  w->table_.reset(new (std::nothrow) uint8_t[table_size]());
  w->raw_.reset(new (std::nothrow) uint8_t[w->frame_bytes_]);
  w->packed_.reset(new (std::nothrow) uint8_t[w->packed_capacity_]);
  if (w->table_ == nullptr || w->raw_ == nullptr || w->packed_ == nullptr) {
    return WT_ERR_NO_MEMORY;
  }
  w->next_offset_ = DataStart(params.frame_count);

  char suffix[48];
  std::snprintf(suffix, sizeof(suffix), ".%ld.%u.tmp",
                static_cast<long>(getpid()),
                g_temp_counter.fetch_add(1, std::memory_order_relaxed));
  w->path_ = path;
  w->temp_path_ = w->path_ + suffix;
  int fd;
  do {
    fd = open(w->temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
              0600);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    w->temp_path_.clear();  // Not ours to unlink.
    return WT_ERR_IO;
  }
  w->fd_ = fd;
  *out = std::move(w);
  return WT_OK;
}

bool WscWriter::WriteAt(const uint8_t* data, size_t size, uint64_t offset) {
  while (size > 0) {
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return false;
    }
    const ssize_t n = pwrite(fd_, data, size, static_cast<off_t>(offset));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    data += n;
    size -= static_cast<size_t>(n);
    offset += static_cast<uint64_t>(n);
  }
  return true;
}

wt_status WscWriter::Append(const YuvaView& frame, uint32_t duration_ms,
                            int64_t pts_ms) {
  if (fd_ < 0) return WT_ERR_IO;
  if (full() || duration_ms == 0 || frame.width != params_.width ||
      frame.height != params_.height || frame.y == nullptr ||
      frame.u == nullptr || frame.v == nullptr ||
      (params_.has_alpha && frame.a == nullptr)) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  const uint32_t cw = ChromaSize(frame.width);
  const uint32_t ch = ChromaSize(frame.height);
  if (frame.y_stride < static_cast<int>(frame.width) ||
      frame.u_stride < static_cast<int>(cw) ||
      frame.v_stride < static_cast<int>(cw) ||
      (params_.has_alpha && frame.a_stride < static_cast<int>(frame.width))) {
    return WT_ERR_INVALID_ARGUMENT;
  }

  // Pack the planes (a view may have row padding), then compress.
  uint8_t* p = raw_.get();
  auto copy_plane = [&p](const uint8_t* src, int stride, uint32_t w,
                         uint32_t h) {
    for (uint32_t row = 0; row < h; ++row) {
      std::memcpy(p, src + size_t{row} * static_cast<size_t>(stride), w);
      p += w;
    }
  };
  copy_plane(frame.y, frame.y_stride, frame.width, frame.height);
  copy_plane(frame.u, frame.u_stride, cw, ch);
  copy_plane(frame.v, frame.v_stride, cw, ch);
  if (params_.has_alpha) {
    copy_plane(frame.a, frame.a_stride, frame.width, frame.height);
  }

  const int n = LZ4_compress_default(reinterpret_cast<const char*>(raw_.get()),
                                     reinterpret_cast<char*>(packed_.get()),
                                     static_cast<int>(frame_bytes_),
                                     static_cast<int>(packed_capacity_));
  if (n <= 0) {
    Abandon();
    return WT_ERR_INTERNAL;
  }
  const uint32_t size = static_cast<uint32_t>(n);
  // next_offset_ + size is the file's size with this frame as its last.
  if (size > max_file_size_ || next_offset_ > max_file_size_ - size) {
    Abandon();
    return WT_ERR_LIMIT;
  }
  if (!WriteAt(packed_.get(), size, next_offset_)) {
    Abandon();
    return WT_ERR_IO;
  }

  uint8_t* e = table_.get() + size_t{written_} * kWscEntrySize;
  Put64(e + kEntOffset, next_offset_);
  Put64(e + kEntPts, static_cast<uint64_t>(pts_ms));
  Put32(e + kEntSize, size);
  Put32(e + kEntCrc, Crc32(0, packed_.get(), size));
  Put32(e + kEntDuration, duration_ms);
  Put32(e + kEntReserved, 0);
  next_offset_ += size;
  ++written_;
  return WT_OK;
}

wt_status WscWriter::Finish() {
  if (fd_ < 0) return WT_ERR_IO;
  if (!full()) return WT_ERR_INVALID_ARGUMENT;
  // The frame buffers aren't needed any more.
  raw_.reset();
  packed_.reset();

  const size_t table_size = size_t{params_.frame_count} * kWscEntrySize;
  uint8_t h[kWscHeaderSize] = {};
  std::memcpy(h, kMagic, sizeof(kMagic));
  Put32(h + kOffVersion, kWscVersion);
  Put32(h + kOffHeaderSize, static_cast<uint32_t>(kWscHeaderSize));
  Put32(h + kOffWidth, params_.width);
  Put32(h + kOffHeight, params_.height);
  Put32(h + kOffFrameCount, params_.frame_count);
  Put32(h + kOffFlags, params_.has_alpha ? kFlagAlpha : 0);
  h[kOffMatrix] = EncodeMatrix(color_.matrix);
  h[kOffFullRange] = color_.full_range ? 1 : 0;
  Put32(h + kOffSourceCrc, params_.source_crc);
  Put64(h + kOffSourceSize, params_.source_size);
  Put64(h + kOffFileSize, next_offset_);
  Put32(h + kOffTableCrc, Crc32(0, table_.get(), table_size));
  Put32(h + kOffHeaderCrc, Crc32(0, h, kOffHeaderCrc));

  if (!WriteAt(table_.get(), table_size, kWscHeaderSize) ||
      !WriteAt(h, sizeof(h), 0) || fsync(fd_) != 0) {
    Abandon();
    return WT_ERR_IO;
  }
  const int fd = fd_;
  fd_ = -1;
  if (close(fd) != 0 || rename(temp_path_.c_str(), path_.c_str()) != 0) {
    Abandon();
    return WT_ERR_IO;
  }
  temp_path_.clear();

  // Make the rename itself durable. Best effort: the file is already in
  // place, and losing it to a crash only costs a re-decode.
  const size_t slash = path_.rfind('/');
  const std::string dir = slash == std::string::npos
                              ? "."
                              : path_.substr(0, slash == 0 ? 1 : slash);
  const int dir_fd = open(dir.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (dir_fd >= 0) {
    fsync(dir_fd);
    close(dir_fd);
  }
  return WT_OK;
}

}  // namespace wt
