// SHA-256 (FIPS 180-4), used only to turn a sticker's cache key into a
// filename-safe, fixed-length name (src/cache/store.h). Not a security
// boundary: the .wsc header fingerprints the source, so a name collision can
// make two stickers share (and keep replacing) one file, but never show the
// wrong frames.
//
// Thread: any thread; no shared mutable state.
#ifndef WT_CACHE_SHA256_H_
#define WT_CACHE_SHA256_H_

#include <cstddef>
#include <cstdint>

namespace wt {

constexpr size_t kSha256Size = 32;

// Hashes size bytes at data (may be null when size is 0) into out.
void Sha256(const uint8_t* data, size_t size, uint8_t out[kSha256Size]);

}  // namespace wt

#endif  // WT_CACHE_SHA256_H_
