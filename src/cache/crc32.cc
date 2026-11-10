#include "cache/crc32.h"

#include <array>

namespace wt {

namespace {

// Slicing-by-8: table[k][b] is the CRC of byte b followed by k zero bytes.
using Tables = std::array<std::array<uint32_t, 256>, 8>;

constexpr Tables MakeTables() {
  Tables t{};
  for (uint32_t b = 0; b < 256; ++b) {
    uint32_t c = b;
    for (int i = 0; i < 8; ++i) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    t[0][b] = c;
  }
  for (uint32_t b = 0; b < 256; ++b) {
    for (size_t k = 1; k < 8; ++k) {
      t[k][b] = (t[k - 1][b] >> 8) ^ t[0][t[k - 1][b] & 0xFF];
    }
  }
  return t;
}

constexpr Tables kTables = MakeTables();

uint32_t Load32(const uint8_t* p) {
  return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 |
         uint32_t{p[3]} << 24;
}

}  // namespace

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
  uint32_t c = ~crc;
  while (size >= 8) {
    const uint32_t lo = Load32(data) ^ c;
    const uint32_t hi = Load32(data + 4);
    c = kTables[7][lo & 0xFF] ^ kTables[6][(lo >> 8) & 0xFF] ^
        kTables[5][(lo >> 16) & 0xFF] ^ kTables[4][lo >> 24] ^
        kTables[3][hi & 0xFF] ^ kTables[2][(hi >> 8) & 0xFF] ^
        kTables[1][(hi >> 16) & 0xFF] ^ kTables[0][hi >> 24];
    data += 8;
    size -= 8;
  }
  while (size-- > 0) c = (c >> 8) ^ kTables[0][(c ^ *data++) & 0xFF];
  return ~c;
}

}  // namespace wt
