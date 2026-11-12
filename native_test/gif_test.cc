// GIF (T5.2): the demuxer and the probe against manifest.json, the delay and
// alpha policies, compositing on crafted files (every disposal method,
// transparency index, palettes, interlacing, clipping, the zero-size screen),
// truncated and hostile files, limits, and wt::Anim playing a GIF (seek, skip,
// frame cache). Pixel accuracy against the reference frames is in
// golden_test.cc.
#include "demux/gif.h"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "anim_util.h"
#include "api/anim.h"
#include "decode/gif_decoder.h"
#include "demux/sniff.h"
#include "gif_writer.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using wt_test::Bytes;
using wt_test::GifPalette;
using wt_test::GifTestFile;
using wt_test::GifTestFrame;
using wt_test::WriteGif;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

Bytes ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
}

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

wt::ByteView View(const Bytes& b) { return wt::ByteView{b.data(), b.size()}; }

struct Owned {
  wt_source* source = nullptr;
  ~Owned() { wt_source_destroy(source); }
};

wt_status Probe(const Bytes& b, wt_info* info,
                const wt_limits* limits = nullptr) {
  Owned o;
  EXPECT_EQ(wt_source_create_memory(b.data(), b.size(), limits, &o.source),
            WT_OK);
  return wt_probe(o.source, info);
}

wt_status Demux(const Bytes& b, wt::GifIndex* index,
                const wt_limits* limits = nullptr) {
  const wt_limits l = limits != nullptr ? *limits : DefaultLimits();
  return wt::DemuxGif(View(b), l, index);
}

const wt_test::Json& Entry() {
  for (const wt_test::Json& f : wt_test::Manifest()["files"].array) {
    if (f["name"].string == "anim_disposal.gif") return f;
  }
  ADD_FAILURE() << "anim_disposal.gif is not in the manifest";
  static const wt_test::Json none;
  return none;
}

// Plays every frame; returns the statuses and per-frame infos.
struct Played {
  std::vector<wt_status> statuses;
  std::vector<wt_frame_info> infos;
  std::vector<Bytes> pixels;
};

Played Play(wt::Anim* anim) {
  Played p;
  const size_t stride = size_t{anim->width()} * 4;
  for (;;) {
    Bytes px(stride * anim->height(), 0xCD);
    wt_frame_info info;
    const wt_status s =
        anim->NextFrame(px.data(), px.size(), static_cast<uint32_t>(stride),
                        WT_PIXEL_FORMAT_RGBA8888, &info);
    if (s == WT_END) break;
    p.statuses.push_back(s);
    p.infos.push_back(info);
    p.pixels.push_back(std::move(px));
  }
  return p;
}

wt_status Create(const Bytes& b, uint32_t w, uint32_t h,
                 std::unique_ptr<wt::Anim>* out,
                 const wt_limits* limits = nullptr,
                 const char* cache_path = nullptr) {
  const wt_limits l = limits != nullptr ? *limits : DefaultLimits();
  return wt::Anim::Create(View(b), wt::Sniff(View(b)), l, w, h, cache_path,
                          out);
}

// --- Crafted inputs ---------------------------------------------------------

using Rgba = std::array<uint8_t, 4>;
constexpr Rgba kClear = {0, 0, 0, 0};
constexpr Rgba kRed = {255, 0, 0, 255};
constexpr Rgba kGreen = {0, 255, 0, 255};
constexpr Rgba kBlue = {0, 0, 255, 255};
constexpr Rgba kYellow = {255, 255, 0, 255};
constexpr Rgba kCyan = {0, 255, 255, 255};

// Palette indices of the 4-colour palette below.
constexpr uint8_t kIRed = 0, kIGreen = 1, kIBlue = 2, kIWhite = 3;

GifPalette P4() {
  return {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
}

GifTestFile File4() {
  GifTestFile f;
  f.screen_width = 4;
  f.screen_height = 4;
  f.global_palette = P4();
  return f;
}

GifTestFrame Solid(int l, int t, int w, int h, uint8_t index, int disposal = 0,
                   int delay_cs = 10) {
  GifTestFrame f;
  f.left = l;
  f.top = t;
  f.width = w;
  f.height = h;
  f.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h), index);
  f.disposal = disposal;
  f.delay_cs = delay_cs;
  return f;
}

// The straight RGBA canvases GifDecoder produces, one per frame. Returns the
// status that ended the run: WT_END after the last frame, or an error.
wt_status DecodeCanvases(const Bytes& gif, std::vector<Bytes>* canvases,
                         wt::GifIndex* index = nullptr,
                         const wt_limits* limits = nullptr) {
  wt::GifIndex local;
  wt::GifIndex* idx = index != nullptr ? index : &local;
  wt_status s = Demux(gif, idx, limits);
  if (s != WT_OK) return s;
  std::unique_ptr<wt::GifDecoder> dec;
  s = wt::GifDecoder::Create(View(gif), *idx, &dec);
  if (s != WT_OK) return s;
  for (;;) {
    wt::RgbaView v;
    s = dec->Next(&v);
    if (s != WT_OK) return s;
    Bytes px;
    for (uint32_t y = 0; y < v.height; ++y) {
      px.insert(px.end(), v.data + y * v.stride,
                v.data + y * v.stride + size_t{v.width} * 4);
    }
    canvases->push_back(std::move(px));
  }
}

Rgba At(const Bytes& canvas, int width, int x, int y) {
  const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                    static_cast<size_t>(x)) *
                   4;
  return {canvas[i], canvas[i + 1], canvas[i + 2], canvas[i + 3]};
}

// Every pixel of a width x height canvas that isn't in `except` has colour c.
void ExpectAllExcept(const Bytes& canvas, int width, int height, Rgba c,
                     const std::vector<std::array<int, 2>>& except) {
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      bool skip = false;
      for (const auto& e : except) skip = skip || (e[0] == x && e[1] == y);
      if (!skip) {
        EXPECT_EQ(At(canvas, width, x, y), c) << x << "," << y;
      }
    }
  }
}

// --- Delay policy -----------------------------------------------------------

TEST(GifDelayTest, Policy) {
  // Browsers play delays under 20 ms (0 and 1 cs) at 100 ms.
  EXPECT_EQ(wt::GifFrameDurationMs(-3), 100u);
  EXPECT_EQ(wt::GifFrameDurationMs(0), 100u);
  EXPECT_EQ(wt::GifFrameDurationMs(1), 100u);
  EXPECT_EQ(wt::GifFrameDurationMs(2), 20u);
  EXPECT_EQ(wt::GifFrameDurationMs(3), 30u);
  EXPECT_EQ(wt::GifFrameDurationMs(10), 100u);
  EXPECT_EQ(wt::GifFrameDurationMs(30), 300u);
  EXPECT_EQ(wt::GifFrameDurationMs(65535), 655350u);
}

TEST(GifDelayTest, CraftedDelaysFollowThePolicy) {
  GifTestFile g = File4();
  for (int cs : {0, 1, 2, 5, 300}) {
    g.frames.push_back(Solid(0, 0, 4, 4, kIRed, 0, cs));
  }
  const Bytes bytes = WriteGif(g);
  wt::GifIndex index;
  wt_limits l = DefaultLimits();
  l.max_duration_ms = 100000;
  ASSERT_EQ(Demux(bytes, &index, &l), WT_OK);
  const std::vector<uint32_t> want = {100, 100, 20, 50, 3000};
  ASSERT_EQ(index.frames.size(), want.size());
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(index.frames[i].duration_ms, want[i]) << i;
  }
  wt_info info;
  ASSERT_EQ(Probe(bytes, &info, &l), WT_OK);
  EXPECT_EQ(info.duration_ms, 3270u);
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim, &l), WT_OK);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.infos.size(), want.size());
  int64_t pts = 0;
  for (size_t i = 0; i < want.size(); ++i) {
    EXPECT_EQ(p.infos[i].duration_ms, want[i]);
    EXPECT_EQ(p.infos[i].pts_ms, pts);
    pts += want[i];
  }
}

// A Graphic Control Extension covers the next image only, and one of the
// wrong length is ignored.
TEST(GifDelayTest, GceAppliesToTheNextImageOnly) {
  GifTestFile g = File4();
  g.frames.push_back(Solid(0, 0, 4, 4, kIRed, /*disposal=*/2, /*delay=*/50));
  GifTestFrame second = Solid(0, 0, 4, 4, kIGreen);
  second.has_gce = false;
  g.frames.push_back(second);
  GifTestFrame third = Solid(0, 0, 4, 4, kIBlue);
  third.has_gce = false;
  // A 3-byte "GCE": ignored, so the defaults apply.
  third.raw_extension_before = {0x21, 0xF9, 3, 0x0C, 0x32, 0x00, 0};
  g.frames.push_back(third);
  wt::GifIndex index;
  ASSERT_EQ(Demux(WriteGif(g), &index), WT_OK);
  ASSERT_EQ(index.frames.size(), 3u);
  EXPECT_EQ(index.frames[0].duration_ms, 500u);
  EXPECT_EQ(index.frames[0].disposal, 2);
  EXPECT_EQ(index.frames[1].duration_ms, 100u);
  EXPECT_EQ(index.frames[1].disposal, 0);
  EXPECT_EQ(index.frames[2].duration_ms, 100u);
  EXPECT_EQ(index.frames[2].disposal, 0);
}

// --- Probe and index of the corpus file -------------------------------------

TEST(GifProbeTest, MatchesManifest) {
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  wt_info info;
  std::memset(&info, 0xAB, sizeof(info));
  ASSERT_EQ(Probe(bytes, &info), WT_OK);
  const wt_test::Json& e = Entry();
  EXPECT_EQ(info.format, WT_FORMAT_GIF);
  EXPECT_EQ(info.codec, WT_CODEC_GIF);
  EXPECT_EQ(info.width, e["width"].Int());
  EXPECT_EQ(info.height, e["height"].Int());
  EXPECT_EQ(info.frame_count, e["frame_count"].Int());
  EXPECT_EQ(info.has_alpha, e["alpha"].boolean);
  EXPECT_FALSE(info.has_audio);
  EXPECT_FALSE(info.truncated);
  int64_t total = 0;
  for (int64_t d : e["durations_ms"].Ints()) total += d;
  EXPECT_EQ(info.duration_ms, total);
  EXPECT_DOUBLE_EQ(info.fps, info.frame_count * 1000.0 / info.duration_ms);
}

TEST(GifProbeTest, PlaybackTimingMatchesManifest) {
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim), WT_OK);
  const std::vector<int64_t> durations = Entry()["durations_ms"].Ints();
  const std::vector<int64_t> pts = Entry()["pts_ms"].Ints();
  const Played p = Play(anim.get());
  ASSERT_EQ(p.infos.size(), durations.size());
  for (size_t i = 0; i < durations.size(); ++i) {
    EXPECT_EQ(p.statuses[i], WT_OK);
    EXPECT_EQ(p.infos[i].index, i);
    EXPECT_EQ(p.infos[i].duration_ms, durations[i]) << i;
    EXPECT_EQ(p.infos[i].pts_ms, pts[i]) << i;
    EXPECT_EQ(anim->frame_duration_ms(static_cast<uint32_t>(i)), durations[i]);
  }
}

// The frames of anim_disposal.gif (see gen_gif in tool/gen_corpus.sh).
TEST(GifProbeTest, IndexHoldsTheCorpusFrames) {
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  wt::GifIndex index;
  ASSERT_EQ(Demux(bytes, &index), WT_OK);
  EXPECT_EQ(index.width, 96u);
  EXPECT_EQ(index.height, 96u);
  EXPECT_TRUE(index.has_alpha);
  EXPECT_FALSE(index.truncated);
  struct Want {
    uint32_t l, t, w, h;
    bool interlaced;
    uint8_t disposal;
  };
  const Want want[] = {{0, 0, 96, 96, false, 1},  {8, 8, 32, 32, false, 2},
                       {56, 8, 32, 32, false, 3}, {32, 28, 36, 40, true, 0},
                       {0, 72, 96, 24, false, 2}, {4, 76, 16, 16, false, 1}};
  ASSERT_EQ(index.frames.size(), 6u);
  for (size_t i = 0; i < 6; ++i) {
    const wt::GifFrame& f = index.frames[i];
    EXPECT_EQ(f.left, want[i].l) << i;
    EXPECT_EQ(f.top, want[i].t) << i;
    EXPECT_EQ(f.width, want[i].w) << i;
    EXPECT_EQ(f.height, want[i].h) << i;
    EXPECT_EQ(f.interlaced, want[i].interlaced) << i;
    EXPECT_EQ(f.disposal, want[i].disposal) << i;
  }
}

// Sniffed but not a whole file: the C ABI agrees with the demuxer.
TEST(GifProbeTest, ProbeAndAnimCreateAgree) {
  const Bytes whole = ReadFile(kAssets / "anim_disposal.gif");
  for (size_t n :
       {size_t{0}, size_t{6}, size_t{12}, size_t{30}, whole.size()}) {
    const Bytes cut(whole.begin(), whole.begin() + static_cast<long>(n));
    wt_info info;
    std::unique_ptr<wt::Anim> anim;
    const wt_status probe = Probe(cut, &info);
    const wt_status create = Create(cut, 0, 0, &anim);
    EXPECT_EQ(probe, create) << n;
  }
}

// --- has_alpha rules --------------------------------------------------------

bool HasAlpha(const GifTestFile& g, wt_status* status = nullptr) {
  wt_info info;
  const wt_status s = Probe(WriteGif(g), &info);
  if (status != nullptr) *status = s;
  EXPECT_EQ(s, WT_OK);
  return info.has_alpha;
}

TEST(GifAlphaTest, Rules) {
  // Fully covering, opaque, nothing exposes the canvas.
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(1, 1, 2, 2, kIGreen, 1),
              Solid(0, 0, 4, 4, kIBlue, 3)};
  EXPECT_FALSE(HasAlpha(g));

  // A transparency index, used or not.
  g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed)};
  g.frames[0].transparent = 3;
  EXPECT_TRUE(HasAlpha(g));

  // A first frame that doesn't cover the canvas.
  g = File4();
  g.frames = {Solid(0, 0, 3, 4, kIRed)};
  EXPECT_TRUE(HasAlpha(g));
  g.frames = {Solid(1, 0, 3, 4, kIRed)};
  EXPECT_TRUE(HasAlpha(g));
  // A rect that sticks out past the canvas still covers it.
  g.frames = {Solid(0, 0, 9, 9, kIRed)};
  EXPECT_FALSE(HasAlpha(g));

  // Background disposal exposes transparency unless it is the last frame's.
  g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed, 2), Solid(0, 0, 4, 4, kIGreen)};
  EXPECT_TRUE(HasAlpha(g));
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(0, 0, 4, 4, kIGreen, 2)};
  EXPECT_FALSE(HasAlpha(g));

  // Restoring the first frame's previous state (transparent), unless last.
  g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed, 3), Solid(0, 0, 2, 2, kIGreen)};
  EXPECT_TRUE(HasAlpha(g));
  g.frames = {Solid(0, 0, 4, 4, kIRed, 3)};
  EXPECT_FALSE(HasAlpha(g));

  // A colour map smaller than the LZW code size allows, or none at all:
  // indices it lacks draw nothing.
  g = File4();
  g.global_palette = {{255, 0, 0}, {0, 255, 0}};
  g.frames = {Solid(0, 0, 4, 4, 1)};
  g.frames[0].min_code_size = 3;
  EXPECT_TRUE(HasAlpha(g));
  g.frames[0].min_code_size = 0;  // 2 colours are coded with 2 bits: normal.
  EXPECT_FALSE(HasAlpha(g));
  g.global_palette.clear();
  EXPECT_TRUE(HasAlpha(g));
}

// --- Compositing ------------------------------------------------------------

TEST(GifComposeTest, LeaveAndNoneKeepTheFrame) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed, 1), Solid(1, 1, 2, 2, kIGreen, 0),
              Solid(0, 0, 1, 1, kIBlue, 0)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 3u);
  ExpectAllExcept(c[0], 4, 4, kRed, {});
  ExpectAllExcept(c[1], 4, 4, kRed, {{1, 1}, {2, 1}, {1, 2}, {2, 2}});
  EXPECT_EQ(At(c[1], 4, 1, 1), kGreen);
  EXPECT_EQ(At(c[2], 4, 0, 0), kBlue);
  EXPECT_EQ(At(c[2], 4, 2, 2), kGreen);
  EXPECT_EQ(At(c[2], 4, 3, 0), kRed);
}

// "Restore to background" clears to transparent, never to the palette's
// background colour or to what was there before.
TEST(GifComposeTest, BackgroundDisposalClearsToTransparent) {
  GifTestFile g = File4();
  g.background = kIWhite;
  g.frames = {Solid(0, 0, 4, 4, kIRed, 2), Solid(1, 1, 2, 2, kIGreen, 2),
              Solid(0, 0, 1, 1, kIBlue, 0)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 3u);
  ExpectAllExcept(c[0], 4, 4, kRed, {});
  ExpectAllExcept(c[1], 4, 4, kClear, {{1, 1}, {2, 1}, {1, 2}, {2, 2}});
  EXPECT_EQ(At(c[1], 4, 2, 2), kGreen);
  ExpectAllExcept(c[2], 4, 4, kClear, {{0, 0}});
  EXPECT_EQ(At(c[2], 4, 0, 0), kBlue);
}

TEST(GifComposeTest, PreviousDisposalRestores) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed, 0), Solid(1, 1, 2, 2, kIGreen, 3),
              Solid(0, 0, 1, 1, kIBlue, 0)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 3u);
  EXPECT_EQ(At(c[1], 4, 1, 1), kGreen);
  // The green block is gone again; the red under it is back.
  ExpectAllExcept(c[2], 4, 4, kRed, {{0, 0}});
  EXPECT_EQ(At(c[2], 4, 0, 0), kBlue);
}

// For the first frame "previous" means the transparent canvas, and a rect that
// sticks out of the canvas restores only its visible part.
TEST(GifComposeTest, PreviousDisposalOfTheFirstFrameAndOfClippedRects) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 2, 2, kIRed, 3), Solid(2, 2, 2, 2, kIGreen, 0)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 2u);
  EXPECT_EQ(At(c[0], 4, 0, 0), kRed);
  EXPECT_EQ(At(c[1], 4, 0, 0), kClear);
  EXPECT_EQ(At(c[1], 4, 1, 1), kClear);
  EXPECT_EQ(At(c[1], 4, 3, 3), kGreen);

  g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(3, 3, 3, 3, kIGreen, 3),
              Solid(0, 0, 1, 1, kIBlue)};
  c.clear();
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 3u);
  EXPECT_EQ(At(c[1], 4, 3, 3), kGreen);
  EXPECT_EQ(At(c[2], 4, 3, 3), kRed);
}

TEST(GifComposeTest, TransparentIndexLeavesTheCanvasBelow) {
  GifTestFile g = File4();
  GifTestFrame top = Solid(0, 0, 4, 4, kIGreen);
  for (size_t i = 0; i < top.pixels.size(); ++i) {
    top.pixels[i] = i % 2 == 0 ? kIGreen : kIBlue;
  }
  top.transparent = kIGreen;
  g.frames = {Solid(0, 0, 4, 4, kIRed), top};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 2u);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      EXPECT_EQ(At(c[1], 4, x, y), (x + y * 4) % 2 == 0 ? kRed : kBlue);
    }
  }
  // As the first frame, the transparent pixels stay transparent.
  g.frames = {top};
  c.clear();
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  EXPECT_EQ(At(c[0], 4, 0, 0), kClear);
  EXPECT_EQ(At(c[0], 4, 1, 0), kBlue);
}

TEST(GifComposeTest, LocalPaletteOverridesTheGlobalOne) {
  GifTestFile g = File4();
  GifTestFrame local = Solid(0, 0, 4, 4, 0);
  local.local_palette = {{255, 255, 0}, {0, 255, 255}};
  for (size_t i = 0; i < local.pixels.size(); ++i) {
    local.pixels[i] = static_cast<uint8_t>(i % 2);
  }
  g.frames = {local, Solid(0, 0, 2, 2, kIRed)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  EXPECT_EQ(At(c[0], 4, 0, 0), kYellow);
  EXPECT_EQ(At(c[0], 4, 1, 0), kCyan);
  // The next frame has none and uses the global map again.
  EXPECT_EQ(At(c[1], 4, 0, 0), kRed);
  EXPECT_EQ(At(c[1], 4, 2, 0), kYellow);

  // No global map at all: the local one is the only palette.
  g.global_palette.clear();
  g.frames = {local};
  c.clear();
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  EXPECT_EQ(At(c[0], 4, 1, 0), kCyan);
}

// Documented policy: a pixel the palette doesn't have draws nothing, as the
// transparency index does.
TEST(GifComposeTest, MissingPaletteEntriesDrawNothing) {
  GifTestFile g = File4();
  g.global_palette = {{255, 0, 0}, {0, 255, 0}};
  GifTestFrame f = Solid(0, 0, 4, 4, 5);  // Past the 2 colours.
  f.min_code_size = 3;
  f.pixels[0] = 1;
  g.frames = {Solid(0, 0, 4, 4, 0), f};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 2u);
  EXPECT_EQ(At(c[1], 4, 0, 0), kGreen);
  ExpectAllExcept(c[1], 4, 4, kRed, {{0, 0}});

  // No colour map at all: the frame is invisible.
  g.global_palette.clear();
  g.frames = {Solid(0, 0, 4, 4, 1)};
  c.clear();
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 1u);
  ExpectAllExcept(c[0], 4, 4, kClear, {});
}

TEST(GifComposeTest, InterlacedFramesAreDeinterlaced) {
  GifPalette pal;
  for (int i = 0; i < 16; ++i) {
    pal.push_back({static_cast<uint8_t>(i * 16), static_cast<uint8_t>(i),
                   static_cast<uint8_t>(255 - i)});
  }
  for (int height = 1; height <= 13; ++height) {
    SCOPED_TRACE(height);
    GifTestFile g;
    g.screen_width = 3;
    g.screen_height = height;
    g.global_palette = pal;
    GifTestFrame f;
    f.width = 3;
    f.height = height;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < 3; ++x) f.pixels.push_back(static_cast<uint8_t>(y));
    }
    f.interlaced = true;
    g.frames = {f};
    std::vector<Bytes> c;
    wt::GifIndex index;
    ASSERT_EQ(DecodeCanvases(WriteGif(g), &c, &index), WT_END);
    ASSERT_EQ(c.size(), 1u);
    EXPECT_TRUE(index.frames[0].interlaced);
    for (int y = 0; y < height; ++y) {
      const Rgba want = {static_cast<uint8_t>(y * 16), static_cast<uint8_t>(y),
                         static_cast<uint8_t>(255 - y), 255};
      for (int x = 0; x < 3; ++x) EXPECT_EQ(At(c[0], 3, x, y), want) << x << y;
    }
  }
}

TEST(GifComposeTest, RectsPastTheScreenAreClipped) {
  GifTestFile g = File4();
  GifTestFrame past = Solid(2, 2, 5, 5, 0);
  for (int y = 0; y < 5; ++y) {
    for (int x = 0; x < 5; ++x) {
      past.pixels[static_cast<size_t>(y * 5 + x)] =
          static_cast<uint8_t>((x + y) % 3);
    }
  }
  g.frames = {Solid(0, 0, 4, 4, kIRed), past,
              Solid(10, 10, 2, 2, kIWhite),  // Wholly outside.
              Solid(0, 0, 6, 6, kIBlue)};
  std::vector<Bytes> c;
  wt::GifIndex index;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c, &index), WT_END);
  ASSERT_EQ(c.size(), 4u);
  const Rgba by_sum[] = {kRed, kGreen, kBlue};
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      EXPECT_EQ(At(c[1], 4, 2 + x, 2 + y), by_sum[(x + y) % 3]);
    }
  }
  EXPECT_EQ(At(c[1], 4, 1, 1), kRed);
  EXPECT_EQ(c[2], c[1]);
  ExpectAllExcept(c[3], 4, 4, kBlue, {});
  EXPECT_EQ(index.width, 4u);
  EXPECT_EQ(index.height, 4u);
}

// A rect is up to 65535 x 65535 whatever the canvas. One that fits the pixel
// budget is read and clipped; one that doesn't is a limit error from the
// probe, which skips pixel data, so it costs nothing.
TEST(GifComposeTest, HugeRectsAreClippedOrRefused) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 60000, 2, kIGreen)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(At(c[0], 4, 3, 1), kGreen);
  EXPECT_EQ(At(c[0], 4, 0, 2), kClear);

  g.frames = {Solid(0, 0, 4, 4, kIRed)};
  g.frames[0].width = 65535;
  g.frames[0].height = 65535;
  g.frames[0].no_pixel_data = true;
  wt::GifIndex index;
  EXPECT_EQ(Demux(WriteGif(g), &index), WT_ERR_LIMIT);
  EXPECT_TRUE(index.frames.empty());
  wt_limits tight = DefaultLimits();
  tight.max_total_pixels = uint64_t{2} << 20;
  tight.max_dimension = 256;
  EXPECT_EQ(Demux(WriteGif(g), &index, &tight), WT_ERR_LIMIT);
}

// A zero-size logical screen takes the far edge of the first frame.
TEST(GifComposeTest, ZeroSizeScreenUsesTheFirstFrame) {
  GifTestFile g;
  g.global_palette = P4();
  g.frames = {Solid(2, 1, 3, 2, kIRed), Solid(0, 0, 9, 9, kIBlue, 0)};
  wt::GifIndex index;
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c, &index), WT_END);
  EXPECT_EQ(index.width, 5u);
  EXPECT_EQ(index.height, 3u);
  EXPECT_TRUE(index.has_alpha) << "the first frame leaves the canvas bare";
  EXPECT_EQ(At(c[0], 5, 0, 0), kClear);
  EXPECT_EQ(At(c[0], 5, 2, 1), kRed);
  ExpectAllExcept(c[1], 5, 3, kBlue, {});

  // One dimension zero: only that one comes from the frame.
  g.screen_height = 4;
  c.clear();
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c, &index), WT_END);
  EXPECT_EQ(index.width, 5u);
  EXPECT_EQ(index.height, 4u);

  // Nothing to take it from, or too big to accept.
  g.screen_height = 0;
  g.frames = {Solid(0, 0, 0, 0, 0)};
  EXPECT_EQ(Demux(WriteGif(g), &index), WT_ERR_INVALID_DATA);
  g.frames = {Solid(0, 0, 2000, 2, 0)};
  g.frames[0].no_pixel_data = true;
  EXPECT_EQ(Demux(WriteGif(g), &index), WT_ERR_LIMIT);
}

TEST(GifComposeTest, EmptyFramesDrawNothing) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(0, 0, 0, 0, 0),
              Solid(1, 1, 1, 1, kIGreen)};
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c), WT_END);
  ASSERT_EQ(c.size(), 3u);
  EXPECT_EQ(c[1], c[0]);
  EXPECT_EQ(At(c[2], 4, 1, 1), kGreen);
}

TEST(GifComposeTest, UndefinedDisposalMethodsMeanNone) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed, 5), Solid(1, 1, 1, 1, kIGreen, 7),
              Solid(2, 2, 1, 1, kIBlue)};
  wt::GifIndex index;
  std::vector<Bytes> c;
  ASSERT_EQ(DecodeCanvases(WriteGif(g), &c, &index), WT_END);
  EXPECT_EQ(index.frames[0].disposal, 0);
  EXPECT_EQ(index.frames[1].disposal, 0);
  EXPECT_EQ(At(c[2], 4, 0, 0), kRed);
  EXPECT_EQ(At(c[2], 4, 1, 1), kGreen);
}

TEST(GifComposeTest, ResetStartsFromATransparentCanvas) {
  GifTestFile g = File4();
  g.frames = {Solid(1, 1, 2, 2, kIRed, 0), Solid(0, 0, 4, 4, kIGreen, 2),
              Solid(0, 0, 1, 1, kIBlue, 0)};
  const Bytes bytes = WriteGif(g);
  wt::GifIndex index;
  ASSERT_EQ(Demux(bytes, &index), WT_OK);
  std::unique_ptr<wt::GifDecoder> dec;
  ASSERT_EQ(wt::GifDecoder::Create(View(bytes), index, &dec), WT_OK);
  std::vector<Bytes> first;
  for (int i = 0; i < 3; ++i) {
    wt::RgbaView v;
    ASSERT_EQ(dec->Next(&v), WT_OK);
    first.emplace_back(v.data, v.data + 64);
  }
  wt::RgbaView v;
  EXPECT_EQ(dec->Next(&v), WT_END);
  EXPECT_EQ(dec->position(), 3u);
  // Reset in the middle of a pass, with a disposal owed: no residue.
  ASSERT_EQ(dec->Reset(), WT_OK);
  EXPECT_EQ(dec->position(), 0u);
  ASSERT_EQ(dec->Next(&v), WT_OK);
  EXPECT_EQ(Bytes(v.data, v.data + 64), first[0]);
  ASSERT_EQ(dec->Reset(), WT_OK);
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(dec->Next(&v), WT_OK);
    EXPECT_EQ(Bytes(v.data, v.data + 64), first[static_cast<size_t>(i)]) << i;
  }
}

// A damaged LZW stream is a decode error for that frame; the decoder then
// refuses frames until Reset, and Anim carries on from a replay.
TEST(GifComposeTest, DecodeErrorLeavesPositionAndNeedsReset) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(0, 0, 4, 4, kIGreen),
              Solid(0, 0, 4, 4, kIBlue)};
  g.frames[1].drop_pixels = 3;
  const Bytes bytes = WriteGif(g);
  wt::GifIndex index;
  ASSERT_EQ(Demux(bytes, &index), WT_OK) << "probing doesn't expand pixels";
  std::unique_ptr<wt::GifDecoder> dec;
  ASSERT_EQ(wt::GifDecoder::Create(View(bytes), index, &dec), WT_OK);
  wt::RgbaView v;
  ASSERT_EQ(dec->Next(&v), WT_OK);
  EXPECT_EQ(dec->Next(&v), WT_ERR_DECODE);
  EXPECT_EQ(dec->position(), 1u);
  EXPECT_EQ(v.data, nullptr);
  EXPECT_EQ(dec->Next(&v), WT_ERR_DECODE);
  EXPECT_EQ(dec->position(), 1u);
  ASSERT_EQ(dec->Reset(), WT_OK);
  ASSERT_EQ(dec->Next(&v), WT_OK);
  EXPECT_EQ(dec->Next(&v), WT_ERR_DECODE);

  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim), WT_OK);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.statuses.size(), 3u);
  EXPECT_EQ(p.statuses[0], WT_OK);
  EXPECT_EQ(p.statuses[1], WT_ERR_DECODE);
  EXPECT_EQ(p.infos.size(), 3u);
  ASSERT_EQ(anim->Seek(0), WT_OK);
  Bytes px(64);
  wt_frame_info info;
  EXPECT_EQ(anim->NextFrame(px.data(), px.size(), 16, WT_PIXEL_FORMAT_RGBA8888,
                            &info),
            WT_OK);
}

// --- Truncated and damaged input --------------------------------------------

// Every prefix of anim_disposal.gif either fails cleanly or plays the
// complete frames before the cut, with the same durations as the whole file.
TEST(GifTruncationTest, EveryPrefixFailsOrPlaysItsCompleteFrames) {
  const Bytes whole = ReadFile(kAssets / "anim_disposal.gif");
  std::vector<uint32_t> durations;
  Played ref;
  {
    std::unique_ptr<wt::Anim> a;
    ASSERT_EQ(Create(whole, 0, 0, &a), WT_OK);
    for (uint32_t i = 0; i < a->info().frame_count; ++i) {
      durations.push_back(a->frame_duration_ms(i));
    }
    ref = Play(a.get());
  }
  uint32_t previous = 0;
  size_t playable = 0;
  for (size_t n = 0; n < whole.size(); ++n) {
    const Bytes cut(whole.begin(), whole.begin() + static_cast<long>(n));
    wt_info info;
    const wt_status s = Probe(cut, &info);
    if (s != WT_OK) {
      EXPECT_TRUE(s == WT_ERR_TRUNCATED || s == WT_ERR_INVALID_DATA)
          << n << ": " << wt_status_string(s);
      continue;
    }
    ++playable;
    ASSERT_TRUE(info.truncated) << n;
    ASSERT_GE(info.frame_count, 1u);
    ASSERT_LE(info.frame_count, durations.size());
    EXPECT_GE(info.frame_count, previous)
        << "frames never shrink with more data";
    previous = info.frame_count;
    std::unique_ptr<wt::Anim> anim;
    ASSERT_EQ(Create(cut, 0, 0, &anim), WT_OK) << n;
    const Played p = Play(anim.get());
    ASSERT_EQ(p.infos.size(), info.frame_count) << n;
    for (size_t i = 0; i < p.infos.size(); ++i) {
      EXPECT_EQ(p.statuses[i], WT_OK) << n << "/" << i;
      EXPECT_EQ(p.infos[i].duration_ms, durations[i]);
      EXPECT_TRUE(p.pixels[i] == ref.pixels[i]) << n << "/" << i;
    }
  }
  EXPECT_GT(playable, 100u) << "most cuts leave complete frames";
  // Without its trailer the whole file still plays all six frames.
  EXPECT_EQ(previous, durations.size());
}

TEST(GifTruncationTest, CutBeforeTheFirstFrameIsTruncated) {
  const Bytes whole = ReadFile(kAssets / "anim_disposal.gif");
  wt_info info;
  EXPECT_EQ(Probe(Bytes(whole.begin(), whole.begin() + 6), &info),
            WT_ERR_TRUNCATED);
  EXPECT_EQ(Probe(Bytes(whole.begin(), whole.begin() + 40), &info),
            WT_ERR_TRUNCATED);
  // The first frame's data is cut: no complete frame.
  EXPECT_EQ(Probe(Bytes(whole.begin(), whole.begin() + 120), &info),
            WT_ERR_TRUNCATED);
}

TEST(GifTruncationTest, MissingTrailerIsTruncatedButPlays) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(0, 0, 4, 4, kIGreen)};
  g.trailer = false;
  wt_info info;
  ASSERT_EQ(Probe(WriteGif(g), &info), WT_OK);
  EXPECT_TRUE(info.truncated);
  EXPECT_EQ(info.frame_count, 2u);
  g.trailer = true;
  ASSERT_EQ(Probe(WriteGif(g), &info), WT_OK);
  EXPECT_FALSE(info.truncated);
}

TEST(GifErrorTest, StructuralDamageIsInvalid) {
  wt_info info;
  // No frames at all.
  GifTestFile g = File4();
  EXPECT_EQ(Probe(WriteGif(g), &info), WT_ERR_INVALID_DATA);
  EXPECT_EQ(info.width, 0u);

  // An LZW code size outside 2..8.
  for (int bad : {1, 9, 12}) {
    g = File4();
    g.frames = {Solid(0, 0, 4, 4, kIRed)};
    g.frames[0].min_code_size = bad;
    EXPECT_EQ(Probe(WriteGif(g), &info), WT_ERR_INVALID_DATA) << bad;
  }

  // An unknown record introducer after a good frame.
  g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed)};
  Bytes b = WriteGif(g);
  b.back() = 0x55;
  EXPECT_EQ(Probe(b, &info), WT_ERR_INVALID_DATA);

  // Header only, and garbage after it.
  b = {'G', 'I', 'F', '8', '9', 'a', 4, 0, 4, 0, 0, 0, 0};
  EXPECT_EQ(Probe(b, &info), WT_ERR_TRUNCATED);
  for (int i = 0; i < 40; ++i) b.push_back(static_cast<uint8_t>(i * 37 + 1));
  const wt_status s = Probe(b, &info);
  EXPECT_TRUE(s == WT_ERR_INVALID_DATA || s == WT_ERR_TRUNCATED)
      << wt_status_string(s);
}

TEST(GifErrorTest, FlippedBytesNeverCrash) {
  const Bytes whole = ReadFile(kAssets / "anim_disposal.gif");
  for (size_t i = 0; i < whole.size(); i += 3) {
    Bytes b = whole;
    b[i] ^= 0xFF;
    for (uint32_t size : {24u, 0u}) {
      std::unique_ptr<wt::Anim> anim;
      if (Create(b, size, size, &anim) != WT_OK) continue;
      const Played p = Play(anim.get());
      for (wt_status s : p.statuses) {
        EXPECT_TRUE(s == WT_OK || s == WT_ERR_DECODE || s == WT_ERR_LIMIT)
            << i << ": " << wt_status_string(s);
      }
    }
  }
}

// --- Limits -----------------------------------------------------------------

TEST(GifLimitTest, EachLimitAtAndPastItsBound) {
  // 96x96, 6 frames, 950 ms; the rects sum to 15184 px.
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  wt_info info;
  wt_limits l = DefaultLimits();
  ASSERT_EQ(Probe(bytes, &info, &l), WT_OK);
  const uint64_t frame_pixels = 96 * 96;

  l = DefaultLimits();
  l.max_dimension = 96;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_dimension = 95;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);
  EXPECT_EQ(info.width, 0u);

  l = DefaultLimits();
  l.max_frames = 6;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_frames = 5;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  l = DefaultLimits();
  l.max_total_pixels = 6 * frame_pixels;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_total_pixels = 6 * frame_pixels - 1;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  l = DefaultLimits();
  l.max_duration_ms = 950;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_duration_ms = 949;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  // wt_anim_create fails the same way.
  l = DefaultLimits();
  l.max_frames = 5;
  std::unique_ptr<wt::Anim> anim;
  EXPECT_EQ(Create(bytes, 0, 0, &anim, &l), WT_ERR_LIMIT);
  EXPECT_EQ(anim, nullptr);

  // Zero is invalid, not unlimited.
  l = DefaultLimits();
  l.max_frames = 0;
  wt::GifIndex index;
  EXPECT_EQ(wt::DemuxGif(View(bytes), l, &index), WT_ERR_INVALID_ARGUMENT);
}

// The pixels the LZW decoder must produce count too: frames whose rects are
// far larger than the canvas can't slip past max_total_pixels.
TEST(GifLimitTest, RectAreaCountsAgainstTheBudget) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 100, 100, kIRed), Solid(0, 0, 100, 100, kIGreen)};
  wt_limits l = DefaultLimits();
  l.max_total_pixels = 20000;
  wt::GifIndex index;
  EXPECT_EQ(Demux(WriteGif(g), &index, &l), WT_OK);
  l.max_total_pixels = 19999;
  EXPECT_EQ(Demux(WriteGif(g), &index, &l), WT_ERR_LIMIT);
}

// Many frames are refused before giflib has held more than max_frames of
// them, and a long file doesn't pile up per-image records.
TEST(GifLimitTest, ManyFramesStayBounded) {
  GifTestFile g;
  g.screen_width = 1;
  g.screen_height = 1;
  g.global_palette = P4();
  for (int i = 0; i < 3000; ++i) g.frames.push_back(Solid(0, 0, 1, 1, 1, 0, 2));
  const Bytes bytes = WriteGif(g);
  wt_limits l = DefaultLimits();
  l.max_duration_ms = 1000000;
  wt::GifIndex index;
  EXPECT_EQ(Demux(bytes, &index, &l), WT_ERR_LIMIT) << "default max_frames";
  l.max_frames = 3000;
  ASSERT_EQ(Demux(bytes, &index, &l), WT_OK);
  EXPECT_EQ(index.frames.size(), 3000u);
  std::vector<Bytes> c;
  EXPECT_EQ(DecodeCanvases(bytes, &c, nullptr, &l), WT_END);
  EXPECT_EQ(c.size(), 3000u);
  l.max_frames = 2999;
  EXPECT_EQ(Demux(bytes, &index, &l), WT_ERR_LIMIT);
}

// The decode budget counts every canvas decoded since the last seek, replay
// after a backward seek included.
TEST(GifLimitTest, DecodeBudgetCountsSeekReplay) {
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  wt_limits l = DefaultLimits();
  l.max_total_pixels = 6 * 96 * 96;  // Exactly one full pass.
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim, &l), WT_OK);
  Bytes px(96 * 96 * 4);
  wt_frame_info info;
  auto next = [&] {
    return anim->NextFrame(px.data(), px.size(), 96 * 4,
                           WT_PIXEL_FORMAT_RGBA8888, &info);
  };
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(anim->Seek(5), WT_OK);
    ASSERT_EQ(next(), WT_OK);  // Six decodes: the budget exactly.
    EXPECT_EQ(info.index, 5u);
  }
  ASSERT_EQ(anim->Seek(3), WT_OK);
  ASSERT_EQ(next(), WT_OK);
  ASSERT_EQ(anim->Seek(2), WT_OK);  // A new pass: reset and decode 0..2.
  ASSERT_EQ(next(), WT_OK);
  // Without a seek in between, a pass can't exceed the budget: play on to the
  // end, then the next pass starts from a Seek.
  for (;;) {
    const wt_status s = next();
    if (s == WT_END) break;
    ASSERT_EQ(s, WT_OK);
  }
}

// --- Playback behaviour -----------------------------------------------------

TEST(GifPlaybackTest, SkipAndSeekMatchSequentialPlayback) {
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  std::unique_ptr<wt::Anim> a;
  ASSERT_EQ(Create(bytes, 0, 0, &a), WT_OK);
  const Played ref = Play(a.get());
  ASSERT_EQ(ref.pixels.size(), 6u);

  std::unique_ptr<wt::Anim> b;
  ASSERT_EQ(Create(bytes, 0, 0, &b), WT_OK);
  const size_t stride = 96 * 4;
  Bytes px(stride * 96);
  wt_frame_info info;
  // Skip frames 0..2 (the compositing state still advances), then read 3.
  for (int i = 0; i < 3; ++i) ASSERT_EQ(b->SkipFrame(&info), WT_OK);
  ASSERT_EQ(b->NextFrame(px.data(), px.size(), stride, WT_PIXEL_FORMAT_RGBA8888,
                         &info),
            WT_OK);
  EXPECT_EQ(info.index, 3u);
  EXPECT_TRUE(px == ref.pixels[3]);
  // Backwards, to the end and again, and the same frame twice.
  for (uint32_t i : {0u, 5u, 2u, 2u, 3u, 1u, 4u}) {
    ASSERT_EQ(b->Seek(i), WT_OK);
    ASSERT_EQ(b->NextFrame(px.data(), px.size(), stride,
                           WT_PIXEL_FORMAT_RGBA8888, &info),
              WT_OK);
    EXPECT_EQ(info.index, i);
    EXPECT_TRUE(px == ref.pixels[i]) << i;
  }
  EXPECT_EQ(b->Seek(6), WT_ERR_INVALID_ARGUMENT);
}

// Alpha comes through the canvas untouched.
TEST(GifPlaybackTest, AlphaIsExactAtSourceSize) {
  GifTestFile g = File4();
  GifTestFrame f = Solid(0, 0, 4, 4, kIGreen);
  f.pixels[5] = kIBlue;
  f.transparent = kIGreen;
  g.frames = {f};
  const Bytes bytes = WriteGif(g);
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim), WT_OK);
  EXPECT_TRUE(anim->info().has_alpha);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.pixels.size(), 1u);
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(p.pixels[0][i * 4 + 3], i == 5 ? 255 : 0) << i;
  }
}

TEST(GifPlaybackTest, OpaqueGifStaysOpaqueScaled) {
  GifTestFile g = File4();
  g.frames = {Solid(0, 0, 4, 4, kIRed), Solid(0, 0, 4, 4, kIBlue)};
  const Bytes bytes = WriteGif(g);
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 2, 2, &anim), WT_OK);
  EXPECT_FALSE(anim->info().has_alpha);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.pixels.size(), 2u);
  for (const Bytes& px : p.pixels) {
    for (size_t i = 3; i < px.size(); i += 4) ASSERT_EQ(px[i], 255);
  }
}

// --- Frame cache ------------------------------------------------------------

class GifCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("wt_gif_cache_" + std::to_string(getpid()));
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  std::string File(const char* name) const { return (dir_ / name).string(); }
  fs::path dir_;
};

TEST_F(GifCacheTest, WritesOnTheFirstPassThenPlaysIdentically) {
  using Mode = wt::Anim::CacheMode;
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  for (uint32_t size : {0u, 48u}) {
    SCOPED_TRACE(size);
    const std::string path = File(size == 0 ? "full.wsc" : "small.wsc");
    std::unique_ptr<wt::Anim> plain;
    ASSERT_EQ(Create(bytes, size, size, &plain), WT_OK);
    EXPECT_EQ(plain->cache_mode(), Mode::kNone);
    const Played ref = Play(plain.get());
    ASSERT_EQ(ref.pixels.size(), 6u);

    std::unique_ptr<wt::Anim> first;
    ASSERT_EQ(Create(bytes, size, size, &first, nullptr, path.c_str()), WT_OK);
    EXPECT_EQ(first->cache_mode(), Mode::kPending);
    const Played written = Play(first.get());
    EXPECT_EQ(first->cache_mode(), Mode::kReading);
    EXPECT_TRUE(fs::exists(path));

    std::unique_ptr<wt::Anim> second;
    ASSERT_EQ(Create(bytes, size, size, &second, nullptr, path.c_str()), WT_OK);
    EXPECT_EQ(second->cache_mode(), Mode::kReading);
    const Played read = Play(second.get());

    for (const Played* p : {&written, &read}) {
      ASSERT_EQ(p->pixels.size(), ref.pixels.size());
      for (size_t i = 0; i < ref.pixels.size(); ++i) {
        EXPECT_TRUE(p->pixels[i] == ref.pixels[i]) << i;
        EXPECT_EQ(p->infos[i].index, ref.infos[i].index);
        EXPECT_EQ(p->infos[i].duration_ms, ref.infos[i].duration_ms);
        EXPECT_EQ(p->infos[i].pts_ms, ref.infos[i].pts_ms);
      }
    }
  }
}

// A cache made from a different file is rejected.
TEST_F(GifCacheTest, ForeignCacheIsReplaced) {
  using Mode = wt::Anim::CacheMode;
  const Bytes bytes = ReadFile(kAssets / "anim_disposal.gif");
  const std::string path = File("c.wsc");
  std::unique_ptr<wt::Anim> a;
  ASSERT_EQ(Create(bytes, 0, 0, &a, nullptr, path.c_str()), WT_OK);
  Play(a.get());
  ASSERT_EQ(a->cache_mode(), Mode::kReading);
  // Same size, other content: flip a byte of pixel data, which probing
  // doesn't read (some offsets land in a header, so look for one that
  // doesn't).
  bool found = false;
  for (size_t at = bytes.size() / 2; at < bytes.size() / 2 + 60 && !found;
       ++at) {
    Bytes other = bytes;
    other[at] ^= 1;
    std::unique_ptr<wt::Anim> b;
    if (Create(other, 0, 0, &b, nullptr, path.c_str()) != WT_OK) continue;
    found = true;
    EXPECT_NE(b->cache_mode(), Mode::kReading);
  }
  EXPECT_TRUE(found);
}

}  // namespace
