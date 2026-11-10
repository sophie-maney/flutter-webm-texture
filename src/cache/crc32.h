// CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320), as zlib's crc32().
// Used by the frame cache (docs/cache-format.md) to detect torn or damaged
// files; it is not a defence against deliberate tampering.
//
// Thread: any thread; no shared mutable state.
#ifndef WT_CACHE_CRC32_H_
#define WT_CACHE_CRC32_H_

#include <cstddef>
#include <cstdint>

namespace wt {

// Continues a CRC over size more bytes. Start with crc = 0; Crc32(0, "", 0)
// is 0 and Crc32(0, "123456789", 9) is 0xCBF43926.
uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size);

}  // namespace wt

#endif  // WT_CACHE_CRC32_H_
