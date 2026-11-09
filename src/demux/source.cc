#include "demux/source.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace wt {
namespace {

class MemorySource final : public ByteSource {
 public:
  static std::unique_ptr<MemorySource> Copy(const uint8_t* data, size_t size) {
    std::unique_ptr<MemorySource> source(new (std::nothrow) MemorySource);
    if (source == nullptr) return nullptr;
    if (size != 0) {
      source->bytes_.reset(new (std::nothrow) uint8_t[size]);
      if (source->bytes_ == nullptr) return nullptr;
      std::memcpy(source->bytes_.get(), data, size);
    }
    source->view_ = ByteView{source->bytes_.get(), size};
    return source;
  }

 private:
  MemorySource() = default;
  std::unique_ptr<uint8_t[]> bytes_;
};

class MappedSource final : public ByteSource {
 public:
  // Takes ownership of an existing mapping of size > 0 bytes.
  static std::unique_ptr<MappedSource> Adopt(void* base, size_t size) {
    std::unique_ptr<MappedSource> source(new (std::nothrow) MappedSource);
    if (source == nullptr) return nullptr;
    source->base_ = base;
    source->view_ = ByteView{static_cast<const uint8_t*>(base), size};
    return source;
  }

  ~MappedSource() override {
    if (base_ != nullptr) munmap(base_, view_.size);
  }

 private:
  MappedSource() = default;
  void* base_ = nullptr;
};

// Closes the descriptor on every exit path. The mapping outlives it.
class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

}  // namespace

wt_status ByteSource::CreateMemory(const uint8_t* data, size_t size,
                                   uint64_t max_bytes,
                                   std::unique_ptr<ByteSource>* out) {
  if (out == nullptr) return WT_ERR_INVALID_ARGUMENT;
  out->reset();
  if (data == nullptr && size != 0) return WT_ERR_INVALID_ARGUMENT;
  if (static_cast<uint64_t>(size) > max_bytes) return WT_ERR_LIMIT;
  std::unique_ptr<MemorySource> source = MemorySource::Copy(data, size);
  if (source == nullptr) return WT_ERR_NO_MEMORY;
  *out = std::move(source);
  return WT_OK;
}

wt_status ByteSource::OpenFile(const char* path, uint64_t max_bytes,
                               std::unique_ptr<ByteSource>* out) {
  if (out == nullptr) return WT_ERR_INVALID_ARGUMENT;
  out->reset();
  if (path == nullptr || path[0] == '\0') return WT_ERR_INVALID_ARGUMENT;

  // O_NONBLOCK so that opening a FIFO or device can't hang; it is rejected
  // below. It has no effect on regular files.
  int raw_fd;
  do {
    raw_fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  } while (raw_fd < 0 && errno == EINTR);
  ScopedFd fd(raw_fd);
  if (fd.get() < 0) return WT_ERR_IO;

  // Check the descriptor, not the path, so the file can't be swapped between
  // the check and the mapping.
  struct stat st;
  if (fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    return WT_ERR_IO;
  }
  const uint64_t file_size = static_cast<uint64_t>(st.st_size);
  if (file_size > max_bytes || file_size > std::numeric_limits<size_t>::max()) {
    return WT_ERR_LIMIT;
  }
  const size_t size = static_cast<size_t>(file_size);

  if (size == 0) {  // mmap rejects a zero length.
    std::unique_ptr<MemorySource> empty = MemorySource::Copy(nullptr, 0);
    if (empty == nullptr) return WT_ERR_NO_MEMORY;
    *out = std::move(empty);
    return WT_OK;
  }

  void* base = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd.get(), 0);
  if (base == MAP_FAILED) return errno == ENOMEM ? WT_ERR_NO_MEMORY : WT_ERR_IO;
  std::unique_ptr<MappedSource> source = MappedSource::Adopt(base, size);
  if (source == nullptr) {
    munmap(base, size);
    return WT_ERR_NO_MEMORY;
  }
  *out = std::move(source);
  return WT_OK;
}

bool ByteSource::Slice(uint64_t offset, uint64_t length, ByteView* out) const {
  if (out != nullptr) *out = ByteView{};
  const uint64_t size = view_.size;
  // Written as two comparisons so offset + length can't wrap around.
  if (offset > size || length > size - offset) return false;
  if (out != nullptr) {
    *out = ByteView{view_.data + offset, static_cast<size_t>(length)};
  }
  return true;
}

bool ByteSource::Read(uint64_t offset, size_t length, void* dst) const {
  ByteView range;
  if (!Slice(offset, length, &range)) return false;
  if (length != 0) {
    if (dst == nullptr) return false;
    std::memcpy(dst, range.data, length);
  }
  return true;
}

}  // namespace wt
