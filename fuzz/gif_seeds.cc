// Writes the fuzz_gif seed corpus: small GIFs crafted with the test writer
// (native_test/gif_writer.h). Built with the fuzzers. The animated corpus file
// test_assets/anim_disposal.gif is the main seed and isn't copied.
//
//   build/fuzz/gif_seeds fuzz/corpus/gif
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../native_test/gif_writer.h"

namespace {

using wt_test::GifPalette;
using wt_test::GifTestFile;
using wt_test::GifTestFrame;

GifPalette Palette() {
  return {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
}

GifTestFrame Frame(int l, int t, int w, int h, int disposal = 0,
                   int delay_cs = 10) {
  GifTestFrame f;
  f.left = l;
  f.top = t;
  f.width = w;
  f.height = h;
  f.disposal = disposal;
  f.delay_cs = delay_cs;
  for (int i = 0; i < w * h; ++i)
    f.pixels.push_back(static_cast<uint8_t>(i % 4));
  return f;
}

GifTestFile File(int w, int h) {
  GifTestFile g;
  g.screen_width = w;
  g.screen_height = h;
  g.global_palette = Palette();
  return g;
}

bool Write(const std::string& dir, const char* name,
           const std::vector<uint8_t>& bytes) {
  const std::string path = dir + "/" + name;
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <out dir>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  bool ok = true;

  GifTestFile g = File(16, 12);
  GifTestFrame interlaced = Frame(0, 0, 16, 12);
  interlaced.interlaced = true;
  g.frames = {interlaced, Frame(2, 2, 6, 6, 2), Frame(4, 4, 8, 8, 3)};
  ok = Write(dir, "interlaced_disposals.gif", wt_test::WriteGif(g)) && ok;

  g = File(8, 8);
  GifTestFrame local = Frame(0, 0, 8, 8);
  local.local_palette = {{255, 255, 0}, {0, 255, 255}};
  for (uint8_t& p : local.pixels) p %= 2;
  local.transparent = 1;
  g.frames = {local, Frame(1, 1, 4, 4)};
  ok = Write(dir, "local_palette_transparent.gif", wt_test::WriteGif(g)) && ok;

  g = File(6, 6);
  GifTestFrame oversized = Frame(3, 3, 9, 9);
  g.frames = {Frame(0, 0, 6, 6), oversized, Frame(20, 20, 2, 2)};
  ok = Write(dir, "oversized_rect.gif", wt_test::WriteGif(g)) && ok;

  g = File(0, 0);
  g.frames = {Frame(2, 1, 5, 3), Frame(0, 0, 9, 9)};
  ok = Write(dir, "zero_screen.gif", wt_test::WriteGif(g)) && ok;

  g = File(8, 8);
  g.frames = {Frame(0, 0, 8, 8, 0, 0), Frame(0, 0, 8, 8, 0, 1),
              Frame(0, 0, 8, 8, 0, 2)};
  ok = Write(dir, "zero_delay.gif", wt_test::WriteGif(g)) && ok;

  g = File(8, 8);
  g.frames = {Frame(2, 2, 4, 4, 3), Frame(0, 0, 8, 8, 3), Frame(5, 5, 6, 6, 3)};
  ok = Write(dir, "previous_first.gif", wt_test::WriteGif(g)) && ok;

  g = File(8, 8);
  g.frames = {Frame(0, 0, 8, 8), Frame(0, 0, 8, 8)};
  g.frames[1].drop_pixels = 5;
  g.frames[1].min_code_size = 3;
  g.trailer = false;
  ok = Write(dir, "short_lzw_no_trailer.gif", wt_test::WriteGif(g)) && ok;

  g = File(8, 8);
  g.frames = {Frame(0, 0, 8, 8)};
  g.frames[0].width = 65535;
  g.frames[0].height = 65535;
  g.frames[0].no_pixel_data = true;
  g.global_palette.clear();
  ok = Write(dir, "huge_rect_no_palette.gif", wt_test::WriteGif(g)) && ok;

  return ok ? 0 : 1;
}
