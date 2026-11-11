// A tiny GIF89a writer for crafting test inputs (native_test/gif_test.cc and
// fuzz/gif_seeds.cc): any rect, disposal, delay, transparency index, local or
// global palette, interlacing, raw extension blocks and deliberate damage.
// The LZW stream is the simplest valid one: every pixel is a literal code and
// a clear code restarts the table before its code width would grow.
#ifndef WT_NATIVE_TEST_GIF_WRITER_H_
#define WT_NATIVE_TEST_GIF_WRITER_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wt_test {

using GifRgb = std::array<uint8_t, 3>;
using GifPalette = std::vector<GifRgb>;  // 2, 4, ..., 256 entries.

struct GifTestFrame {
  int left = 0, top = 0, width = 0, height = 0;
  std::vector<uint8_t> pixels;  // width * height palette indices, raster order.
  bool interlaced = false;
  GifPalette local_palette;  // Empty: use the global one.
  bool has_gce = true;
  int disposal = 0;            // GCE disposal method, 0..7.
  int delay_cs = 10;           // GCE delay.
  int transparent = -1;        // GCE transparency index, or -1.
  int min_code_size = 0;       // 0: derived from the palette (>= 2).
  size_t drop_pixels = 0;      // End the LZW stream this many pixels early.
  bool no_pixel_data = false;  // An empty LZW stream whatever the rect says.
  std::vector<uint8_t> raw_extension_before;  // Written verbatim.
};

struct GifTestFile {
  int screen_width = 0, screen_height = 0;
  GifPalette global_palette;  // Empty: no global colour map.
  int background = 0;
  std::vector<GifTestFrame> frames;
  bool trailer = true;
};

namespace gif_detail {

inline void Put16(std::vector<uint8_t>* out, int v) {
  out->push_back(static_cast<uint8_t>(v & 0xFF));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// log2(size) - 1 for a palette of 2^k entries, as the packed field wants.
inline int SizeField(size_t entries) {
  int k = 1;
  while ((size_t{1} << k) < entries) ++k;
  return k - 1;
}

inline void PutPalette(std::vector<uint8_t>* out, const GifPalette& p) {
  const size_t entries = size_t{2} << SizeField(p.size());
  for (size_t i = 0; i < entries; ++i) {
    const GifRgb c = i < p.size() ? p[i] : GifRgb{0, 0, 0};
    out->insert(out->end(), c.begin(), c.end());
  }
}

// The row of the image stored at stream position r.
inline std::vector<int> RowOrder(int height, bool interlaced) {
  std::vector<int> order;
  if (!interlaced) {
    for (int y = 0; y < height; ++y) order.push_back(y);
    return order;
  }
  const int start[] = {0, 4, 2, 1}, step[] = {8, 8, 4, 2};
  for (int pass = 0; pass < 4; ++pass) {
    for (int y = start[pass]; y < height; y += step[pass]) order.push_back(y);
  }
  return order;
}

inline void PutLzw(std::vector<uint8_t>* out, const std::vector<uint8_t>& px,
                   int min_code_size) {
  out->push_back(static_cast<uint8_t>(min_code_size));
  const int width = min_code_size + 1;
  const unsigned clear = 1u << min_code_size, eoi = clear + 1;
  // Literals between clears: the table stays below 2^width entries.
  const size_t run = std::max<size_t>(1, (size_t{1} << min_code_size) - 2);
  std::vector<uint8_t> bytes;
  uint32_t acc = 0;
  int bits = 0;
  auto put = [&](unsigned code) {
    acc |= code << bits;
    bits += width;
    while (bits >= 8) {
      bytes.push_back(static_cast<uint8_t>(acc & 0xFF));
      acc >>= 8;
      bits -= 8;
    }
  };
  put(clear);
  for (size_t i = 0; i < px.size(); ++i) {
    if (i != 0 && i % run == 0) put(clear);
    put(px[i]);
  }
  put(eoi);
  if (bits > 0) bytes.push_back(static_cast<uint8_t>(acc & 0xFF));
  for (size_t at = 0; at < bytes.size(); at += 255) {
    const size_t n = std::min<size_t>(255, bytes.size() - at);
    out->push_back(static_cast<uint8_t>(n));
    out->insert(out->end(), bytes.begin() + static_cast<long>(at),
                bytes.begin() + static_cast<long>(at + n));
  }
  out->push_back(0);
}

}  // namespace gif_detail

inline std::vector<uint8_t> WriteGif(const GifTestFile& g) {
  using namespace gif_detail;
  std::vector<uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
  Put16(&out, g.screen_width);
  Put16(&out, g.screen_height);
  const bool has_gct = !g.global_palette.empty();
  out.push_back(static_cast<uint8_t>(
      (has_gct ? 0x80 | 0x70 | SizeField(g.global_palette.size()) : 0x70)));
  out.push_back(static_cast<uint8_t>(g.background));
  out.push_back(0);
  if (has_gct) PutPalette(&out, g.global_palette);

  for (const GifTestFrame& f : g.frames) {
    out.insert(out.end(), f.raw_extension_before.begin(),
               f.raw_extension_before.end());
    if (f.has_gce) {
      out.push_back(0x21);
      out.push_back(0xF9);
      out.push_back(4);
      out.push_back(static_cast<uint8_t>((f.disposal & 7) << 2 |
                                         (f.transparent >= 0 ? 1 : 0)));
      Put16(&out, f.delay_cs);
      out.push_back(
          static_cast<uint8_t>(f.transparent >= 0 ? f.transparent : 0));
      out.push_back(0);
    }
    out.push_back(0x2C);
    Put16(&out, f.left);
    Put16(&out, f.top);
    Put16(&out, f.width);
    Put16(&out, f.height);
    const bool local = !f.local_palette.empty();
    out.push_back(static_cast<uint8_t>(
        (local ? 0x80 | SizeField(f.local_palette.size()) : 0) |
        (f.interlaced ? 0x40 : 0)));
    if (local) PutPalette(&out, f.local_palette);
    const GifPalette& pal = local ? f.local_palette : g.global_palette;
    int min_code = f.min_code_size;
    if (min_code == 0) {
      min_code = std::max(2, SizeField(pal.empty() ? 4 : pal.size()) + 1);
    }
    std::vector<uint8_t> stream;
    if (!f.no_pixel_data) {
      for (int y : RowOrder(f.height, f.interlaced)) {
        for (int x = 0; x < f.width; ++x) {
          const size_t at =
              static_cast<size_t>(y) * static_cast<size_t>(f.width) +
              static_cast<size_t>(x);
          stream.push_back(at < f.pixels.size() ? f.pixels[at] : 0);
        }
      }
    }
    stream.resize(stream.size() - std::min(stream.size(), f.drop_pixels));
    PutLzw(&out, stream, min_code);
  }
  if (g.trailer) out.push_back(0x3B);
  return out;
}

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_GIF_WRITER_H_
