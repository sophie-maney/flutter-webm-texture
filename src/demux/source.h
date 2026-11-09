// Byte sources: the immutable, bounds-checked input every demuxer reads.
//
// A source is either a private copy of caller memory or a read-only mapping of
// a file. Both expose one contiguous range, so demuxers and decoders can read
// it without copying. All access goes through Slice()/Read(), which check
// offset and length against the size without overflowing: sticker files come
// from other users, and the offsets inside them are untrusted.
//
// Thread: a ByteSource never changes after creation, so any number of threads
// may read it concurrently. Creating one may do file I/O or copy up to
// max_bytes, so call the factories from a background thread.
#ifndef WT_DEMUX_SOURCE_H_
#define WT_DEMUX_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "api/wt_api.h"

namespace wt {

// A non-owning range of bytes. Valid while the ByteSource it came from lives.
struct ByteView {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

class ByteSource {
 public:
  virtual ~ByteSource() = default;
  ByteSource(const ByteSource&) = delete;
  ByteSource& operator=(const ByteSource&) = delete;

  // Copies size bytes at data (data may be null only when size is 0).
  // Returns WT_ERR_LIMIT when size > max_bytes (nothing is copied) and
  // WT_ERR_NO_MEMORY when the copy can't be allocated.
  static wt_status CreateMemory(const uint8_t* data, size_t size,
                                uint64_t max_bytes,
                                std::unique_ptr<ByteSource>* out);

  // Maps the file at path (UTF-8) read-only. The size is checked against
  // max_bytes before mapping. Returns WT_ERR_IO when the path can't be opened
  // or mapped or isn't a regular file, and WT_ERR_LIMIT when it is too big.
  // The file must not shrink while the source is alive (reading a page past
  // the new end raises SIGBUS); callers write files under a temporary name
  // and rename them into place.
  static wt_status OpenFile(const char* path, uint64_t max_bytes,
                            std::unique_ptr<ByteSource>* out);

  size_t size() const { return view_.size; }
  ByteView whole() const { return view_; }

  // Returns the length bytes starting at offset. False, with *out emptied,
  // when the range isn't entirely inside the source.
  bool Slice(uint64_t offset, uint64_t length, ByteView* out) const;

  // Copies length bytes starting at offset into dst. False, leaving dst
  // untouched, when the range isn't entirely inside the source.
  bool Read(uint64_t offset, size_t length, void* dst) const;

 protected:
  ByteSource() = default;
  ByteView view_;  // Set once by the factory before the source is returned.
};

}  // namespace wt

#endif  // WT_DEMUX_SOURCE_H_
