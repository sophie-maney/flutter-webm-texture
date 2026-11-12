// Tests for the VP8/VP9 decoder (src/decode/vpx_decoder.*) and the YUVA
// conversions (src/convert/yuva.*): alpha gaps and resynchronisation,
// keyframe gating, size checks, colour matrices, scaling and buffer checks.
#include <sys/resource.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "convert/yuva.h"
#include "decode/vpx_decoder.h"
#include "demux/webm.h"
#include "gtest/gtest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

struct Clip {
  Bytes bytes;
  wt::WebmIndex index;

  wt::ByteView Data(size_t i) const { return View(index.frames[i].data); }
  wt::ByteView Alpha(size_t i) const { return View(index.frames[i].alpha); }
  wt::ByteView View(wt::Span s) const {
    return s.empty() ? wt::ByteView{}
                     : wt::ByteView{bytes.data() + s.offset,
                                    static_cast<size_t>(s.size)};
  }
};

void LoadClip(const char* name, Clip* clip) {
  std::ifstream in(kAssets / name, std::ios::binary);
  clip->bytes.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
  wt_limits limits;
  wt_limits_init_default(&limits);
  ASSERT_EQ(wt::DemuxWebm(wt::ByteView{clip->bytes.data(), clip->bytes.size()},
                          limits, &clip->index),
            WT_OK);
}

wt_status InitFor(const Clip& clip, wt::VpxDecoder* decoder) {
  const wt::WebmVideoTrack& t = clip.index.video;
  return decoder->Init(t.codec, t.width, t.height, t.has_alpha());
}

// --- VpxDecoder ------------------------------------------------------------

TEST(VpxDecoderTest, InitArguments) {
  wt::VpxDecoder d;
  EXPECT_EQ(d.Init(WT_CODEC_WEBP, 64, 64, false), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(d.Init(WT_CODEC_VP9, 0, 64, false), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(d.Init(WT_CODEC_VP9, 64, 20000, false), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(d.Init(WT_CODEC_VP9, 64, 64, false), WT_OK);
  EXPECT_EQ(d.Init(WT_CODEC_VP9, 64, 64, false), WT_ERR_INVALID_ARGUMENT)
      << "only once";

  wt::VpxDecoder uninitialised;
  wt::VpxDecoder::Output out;
  const uint8_t byte = 0;
  EXPECT_EQ(uninitialised.Decode(wt::ByteView{&byte, 1}, {}, &out),
            WT_ERR_INTERNAL);
}

TEST(VpxDecoderTest, EveryFrameShowsColourAndAlpha) {
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm"}) {
    SCOPED_TRACE(name);
    Clip clip;
    ASSERT_NO_FATAL_FAILURE(LoadClip(name, &clip));
    wt::VpxDecoder d;
    ASSERT_EQ(InitFor(clip, &d), WT_OK);
    for (size_t i = 0; i < clip.index.frames.size(); ++i) {
      wt::VpxDecoder::Output out;
      ASSERT_EQ(d.Decode(clip.Data(i), clip.Alpha(i), &out), WT_OK) << i;
      EXPECT_TRUE(out.shown);
      EXPECT_TRUE(out.alpha_fresh);
      EXPECT_NE(out.view.a, nullptr);
      EXPECT_EQ(out.view.width, clip.index.video.width);
      EXPECT_EQ(out.view.height, clip.index.video.height);
      // The corpus is tagged BT.601 studio range (VP8 reports "unknown").
      EXPECT_EQ(out.view.color.matrix, wt::YuvMatrix::kBt601);
      EXPECT_FALSE(out.view.color.full_range);
    }
  }
}

// Spike A, pitfall 2: a frame that loses its alpha payload, or whose alpha
// fails to decode, keeps the frame but skips alpha until the next alpha
// keyframe (VP8 would otherwise carry on with silently wrong alpha).
TEST(VpxDecoderTest, AlphaGapResumesAtTheNextAlphaKeyframe) {
  struct Case {
    const char* name;
    size_t next_keyframe;
  };
  const Bytes junk(64, 0x5A);
  for (const Case& c :
       {Case{"vp8_alpha_256.webm", 30}, Case{"vp9_alpha_512.webm", 45}}) {
    for (bool corrupt : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (corrupt ? " corrupt" : " missing"));
      Clip clip;
      ASSERT_NO_FATAL_FAILURE(LoadClip(c.name, &clip));
      wt::VpxDecoder d;
      ASSERT_EQ(InitFor(clip, &d), WT_OK);
      for (size_t i = 0; i < clip.index.frames.size(); ++i) {
        wt::ByteView alpha = clip.Alpha(i);
        if (i == 10) {
          alpha =
              corrupt ? wt::ByteView{junk.data(), junk.size()} : wt::ByteView{};
        }
        wt::VpxDecoder::Output out;
        ASSERT_EQ(d.Decode(clip.Data(i), alpha, &out), WT_OK) << i;
        EXPECT_TRUE(out.shown) << i;
        const bool expect_alpha = i < 10 || i >= c.next_keyframe;
        EXPECT_EQ(out.alpha_fresh, expect_alpha) << i;
        EXPECT_EQ(out.view.a != nullptr, expect_alpha) << i;
      }
    }
  }
}

TEST(VpxDecoderTest, AlphaThatStartsLateWaitsForAKeyframe) {
  Clip clip;
  ASSERT_NO_FATAL_FAILURE(LoadClip("vp8_alpha_256.webm", &clip));
  wt::VpxDecoder d;
  ASSERT_EQ(InitFor(clip, &d), WT_OK);
  for (size_t i = 0; i < clip.index.frames.size(); ++i) {
    wt::VpxDecoder::Output out;
    const wt::ByteView alpha = i >= 5 ? clip.Alpha(i) : wt::ByteView{};
    ASSERT_EQ(d.Decode(clip.Data(i), alpha, &out), WT_OK) << i;
    EXPECT_EQ(out.alpha_fresh, i >= 30) << i;
  }
}

TEST(VpxDecoderTest, OpaqueTrackIgnoresAlphaPayloads) {
  Clip clip;
  ASSERT_NO_FATAL_FAILURE(LoadClip("vp9_alpha_vfr.webm", &clip));
  wt::VpxDecoder d;
  const wt::WebmVideoTrack& t = clip.index.video;
  ASSERT_EQ(d.Init(t.codec, t.width, t.height, /*track_alpha=*/false), WT_OK);
  for (size_t i = 0; i < 5; ++i) {
    wt::VpxDecoder::Output out;
    ASSERT_EQ(d.Decode(clip.Data(i), clip.Alpha(i), &out), WT_OK);
    EXPECT_TRUE(out.shown);
    EXPECT_FALSE(out.alpha_fresh);
    EXPECT_EQ(out.view.a, nullptr);
  }
}

// Colour decoding starts only at a keyframe, and restarts at one after a
// failure, instead of decoding inter frames against missing references.
TEST(VpxDecoderTest, ColourWaitsForAKeyframe) {
  for (const char* name : {"vp8_alpha_256.webm", "vp9_alpha_512.webm"}) {
    SCOPED_TRACE(name);
    Clip clip;
    ASSERT_NO_FATAL_FAILURE(LoadClip(name, &clip));
    wt::VpxDecoder d;
    ASSERT_EQ(InitFor(clip, &d), WT_OK);
    wt::VpxDecoder::Output out;
    EXPECT_EQ(d.Decode(clip.Data(1), clip.Alpha(1), &out), WT_ERR_DECODE);
    EXPECT_FALSE(out.shown);
    EXPECT_EQ(d.Decode(clip.Data(0), clip.Alpha(0), &out), WT_OK);
    EXPECT_TRUE(out.shown);
    EXPECT_TRUE(out.alpha_fresh);

    // A corrupt colour packet fails, and so does everything up to the next
    // keyframe; the alpha stream restarts there too.
    const Bytes junk(48, 0xA5);
    EXPECT_EQ(
        d.Decode(wt::ByteView{junk.data(), junk.size()}, clip.Alpha(1), &out),
        WT_ERR_DECODE);
    EXPECT_EQ(d.Decode(clip.Data(2), clip.Alpha(2), &out), WT_ERR_DECODE);
    EXPECT_EQ(d.Decode(clip.Data(0), clip.Alpha(0), &out), WT_OK);
    EXPECT_TRUE(out.alpha_fresh);
    EXPECT_EQ(d.Decode(clip.Data(1), clip.Alpha(1), &out), WT_OK);

    EXPECT_EQ(d.Decode(wt::ByteView{}, {}, &out), WT_ERR_DECODE);
  }
}

// Spike A, pitfall 6: the decoded size must be the track's.
TEST(VpxDecoderTest, SizeMismatchIsInvalidData) {
  Clip clip;
  ASSERT_NO_FATAL_FAILURE(LoadClip("vp9_alpha_vfr.webm", &clip));
  wt::VpxDecoder d;
  ASSERT_EQ(d.Init(WT_CODEC_VP9, 192, 191, true), WT_OK);
  wt::VpxDecoder::Output out;
  EXPECT_EQ(d.Decode(clip.Data(0), clip.Alpha(0), &out), WT_ERR_INVALID_DATA);
  EXPECT_FALSE(out.shown);
  EXPECT_EQ(out.view.y, nullptr);
}

// A VP8 keyframe whose header claims width x height (14 bits each), with a
// zeroed first partition: all libvpx needs to size its frame buffers.
Bytes Vp8Keyframe(uint32_t width, uint32_t height) {
  const uint32_t tag = 64u << 5 | 1u << 4;  // Keyframe, shown, 64-byte part.
  Bytes b = {static_cast<uint8_t>(tag),
             static_cast<uint8_t>(tag >> 8),
             static_cast<uint8_t>(tag >> 16),
             0x9d,
             0x01,
             0x2a,
             static_cast<uint8_t>(width),
             static_cast<uint8_t>((width >> 8) & 0x3f),
             static_cast<uint8_t>(height),
             static_cast<uint8_t>((height >> 8) & 0x3f)};
  b.resize(b.size() + 128, 0);
  return b;
}

uint64_t PeakRssBytes() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return static_cast<uint64_t>(usage.ru_maxrss);  // Bytes.
#else
  return static_cast<uint64_t>(usage.ru_maxrss) * 1024;  // KiB.
#endif
}

// T6.1 (S-01): libvpx's --size-limit guards VP9 only. VP8 allocated four
// frame buffers at a keyframe header's size (up to 16383 x 16383, ~400 MB
// each) before decoding anything, so a few hundred bytes in a 16 x 16 track
// took ~465 MB of RSS. Such a keyframe never reaches libvpx now, and the
// decoder still takes the next keyframe of the right size. (The RSS bound
// only bites when ctest runs this test in its own process.)
TEST(VpxDecoderTest, OversizedVp8KeyframeIsRefusedBeforeDecoding) {
  Clip clip;
  ASSERT_NO_FATAL_FAILURE(LoadClip("vp8_alpha_256.webm", &clip));
  wt::VpxDecoder d;
  ASSERT_EQ(InitFor(clip, &d), WT_OK);
  const uint64_t rss_before = PeakRssBytes();
  const Bytes bomb = Vp8Keyframe(16383, 16383);
  const wt::ByteView view{bomb.data(), bomb.size()};
  wt::VpxDecoder::Output out;
  EXPECT_EQ(d.Decode(view, view, &out), WT_ERR_INVALID_DATA);
  EXPECT_FALSE(out.shown);
  EXPECT_LT(PeakRssBytes() - rss_before, uint64_t{64} << 20);
  // A right-size colour keyframe with an oversized alpha one: the colour
  // shows, the alpha is refused like a broken payload.
  EXPECT_EQ(d.Decode(clip.Data(0), view, &out), WT_OK);
  EXPECT_TRUE(out.shown);
  EXPECT_FALSE(out.alpha_fresh);
  EXPECT_LT(PeakRssBytes() - rss_before, uint64_t{64} << 20);
  EXPECT_EQ(d.Decode(clip.Data(0), clip.Alpha(0), &out), WT_OK);
  EXPECT_TRUE(out.alpha_fresh);
}

TEST(VpxDecoderTest, WrongCodecFails) {
  Clip clip;
  ASSERT_NO_FATAL_FAILURE(LoadClip("vp9_alpha_vfr.webm", &clip));
  wt::VpxDecoder d;
  ASSERT_EQ(d.Init(WT_CODEC_VP8, 192, 192, true), WT_OK);
  wt::VpxDecoder::Output out;
  EXPECT_EQ(d.Decode(clip.Data(0), clip.Alpha(0), &out), WT_ERR_DECODE);
}

// --- YUVA conversion -------------------------------------------------------

// A width x height YUVA image with constant planes.
struct Planes {
  Bytes y, u, v, a;
  wt::YuvaView view;

  Planes(uint32_t w, uint32_t h, uint8_t yv, uint8_t uv, uint8_t vv,
         int av /* -1: no alpha */) {
    const uint32_t cw = wt::ChromaSize(w), ch = wt::ChromaSize(h);
    y.assign(size_t{w} * h, yv);
    u.assign(size_t{cw} * ch, uv);
    v.assign(size_t{cw} * ch, vv);
    view.y = y.data();
    view.u = u.data();
    view.v = v.data();
    view.y_stride = static_cast<int>(w);
    view.u_stride = view.v_stride = static_cast<int>(cw);
    if (av >= 0) {
      a.assign(size_t{w} * h, static_cast<uint8_t>(av));
      view.a = a.data();
      view.a_stride = static_cast<int>(w);
    }
    view.width = w;
    view.height = h;
  }
};

Bytes Convert(const wt::YuvaView& view, wt_pixel_format format) {
  Bytes out(size_t{view.width} * view.height * 4, 0xCD);
  EXPECT_EQ(wt::YuvaToPremultiplied(view, out.data(), out.size(),
                                    view.width * 4, format),
            WT_OK);
  return out;
}

TEST(YuvaConvertTest, OpaqueHasFullAlpha) {
  Planes p(5, 3, 235, 128, 128, -1);
  for (wt_pixel_format f :
       {WT_PIXEL_FORMAT_RGBA8888, WT_PIXEL_FORMAT_BGRA8888}) {
    const Bytes px = Convert(p.view, f);
    for (size_t i = 0; i < px.size(); i += 4) {
      EXPECT_EQ(px[i + 3], 255);
      EXPECT_GE(px[i], 254);  // Studio white.
    }
  }
}

TEST(YuvaConvertTest, AlphaPremultiplies) {
  Planes p(4, 4, 235, 128, 128, 128);
  const Bytes px = Convert(p.view, WT_PIXEL_FORMAT_RGBA8888);
  for (size_t i = 0; i < px.size(); i += 4) {
    EXPECT_EQ(px[i + 3], 128);
    EXPECT_NEAR(px[i], 128, 1);
    EXPECT_NEAR(px[i + 1], 128, 1);
    EXPECT_NEAR(px[i + 2], 128, 1);
  }
  Planes clear(4, 4, 235, 90, 200, 0);
  for (uint8_t b : Convert(clear.view, WT_PIXEL_FORMAT_BGRA8888)) {
    EXPECT_EQ(b, 0);
  }
}

TEST(YuvaConvertTest, RangeAndMatrixFollowTheStream) {
  // Mid grey: studio range maps Y 128 to about 130, full range to 128.
  Planes grey(2, 2, 128, 128, 128, -1);
  grey.view.color.full_range = false;
  EXPECT_NEAR(Convert(grey.view, WT_PIXEL_FORMAT_RGBA8888)[0], 130, 1);
  grey.view.color.full_range = true;
  EXPECT_NEAR(Convert(grey.view, WT_PIXEL_FORMAT_RGBA8888)[0], 128, 1);

  // A saturated colour converts differently under each matrix.
  Planes red(2, 2, 81, 90, 240, -1);
  Bytes results[3];
  const wt::YuvMatrix matrices[] = {
      wt::YuvMatrix::kBt601, wt::YuvMatrix::kBt709, wt::YuvMatrix::kBt2020};
  for (int m = 0; m < 3; ++m) {
    red.view.color.matrix = matrices[m];
    results[m] = Convert(red.view, WT_PIXEL_FORMAT_RGBA8888);
  }
  EXPECT_NEAR(results[0][0], 255, 2) << "BT.601 red";
  EXPECT_NEAR(results[0][1], 0, 2);
  EXPECT_NE(results[0], results[1]);
  EXPECT_NE(results[1], results[2]);

  // BGRA is RGBA with red and blue swapped, under every matrix and range.
  for (bool full : {false, true}) {
    for (wt::YuvMatrix m : matrices) {
      red.view.color = wt::YuvColor{m, full};
      Bytes rgba = Convert(red.view, WT_PIXEL_FORMAT_RGBA8888);
      const Bytes bgra = Convert(red.view, WT_PIXEL_FORMAT_BGRA8888);
      for (size_t i = 0; i < rgba.size(); i += 4)
        std::swap(rgba[i], rgba[i + 2]);
      EXPECT_EQ(rgba, bgra);
    }
  }
}

TEST(YuvaConvertTest, Arguments) {
  Planes p(4, 4, 16, 128, 128, 255);
  Bytes out(64);
  EXPECT_EQ(wt::YuvaToPremultiplied(p.view, nullptr, 64, 16,
                                    WT_PIXEL_FORMAT_RGBA8888),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::YuvaToPremultiplied(p.view, out.data(), 64, 16, 7),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::YuvaToPremultiplied(p.view, out.data(), 63, 16,
                                    WT_PIXEL_FORMAT_RGBA8888),
            WT_ERR_INVALID_ARGUMENT);
  wt::YuvaView empty;
  EXPECT_EQ(wt::YuvaToPremultiplied(empty, out.data(), 64, 16,
                                    WT_PIXEL_FORMAT_RGBA8888),
            WT_ERR_INVALID_ARGUMENT);
  wt::YuvaView short_stride = p.view;
  short_stride.u_stride = 1;
  EXPECT_EQ(wt::YuvaToPremultiplied(short_stride, out.data(), 64, 16,
                                    WT_PIXEL_FORMAT_RGBA8888),
            WT_ERR_INVALID_ARGUMENT);
}

TEST(YuvaConvertTest, CheckPixelBuffer) {
  EXPECT_EQ(wt::CheckPixelBuffer(4, 2, 32, 16), WT_OK);
  EXPECT_EQ(wt::CheckPixelBuffer(4, 2, 31, 16), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CheckPixelBuffer(4, 2, 36, 20), WT_OK) << "last row unpadded";
  EXPECT_EQ(wt::CheckPixelBuffer(4, 2, 35, 20), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CheckPixelBuffer(4, 2, 32, 15), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CheckPixelBuffer(0, 2, 32, 16), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CheckPixelBuffer(4, 0, 32, 16), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CheckPixelBuffer(1, 1, SIZE_MAX, 0x80000000u),
            WT_ERR_INVALID_ARGUMENT)
      << "stride beyond int";
  EXPECT_EQ(wt::CheckPixelBuffer(1024, 1024, SIZE_MAX, 0x7FFFFFFFu), WT_OK);
  EXPECT_EQ(wt::CheckPixelBuffer(1024, 1024, 0xFFFFFFFFu, 0x7FFFFFFFu),
            WT_ERR_INVALID_ARGUMENT)
      << "needs more than 32 bits";
  EXPECT_EQ(wt::CheckPixelBuffer(UINT32_MAX, 1, SIZE_MAX, 0x7FFFFFFFu),
            WT_ERR_INVALID_ARGUMENT)
      << "row wider than any stride";
}

TEST(YuvaImageTest, Allocate) {
  wt::YuvaImage img;
  EXPECT_EQ(img.Allocate(0, 4, false), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(img.Allocate(4, wt::YuvaImage::kMaxDimension + 1, false),
            WT_ERR_INVALID_ARGUMENT);
  ASSERT_EQ(img.Allocate(5, 3, true), WT_OK);
  const wt::YuvaView v = img.view();
  EXPECT_EQ(v.width, 5u);
  EXPECT_EQ(v.height, 3u);
  EXPECT_EQ(v.u_stride, 3);
  ASSERT_NE(v.a, nullptr);
  // Starts opaque black.
  const Bytes px = Convert(v, WT_PIXEL_FORMAT_RGBA8888);
  for (size_t i = 0; i < px.size(); i += 4) {
    EXPECT_EQ(px[i], 0);
    EXPECT_EQ(px[i + 3], 255);
  }
  ASSERT_EQ(img.Allocate(2, 2, false), WT_OK);
  EXPECT_EQ(img.view().a, nullptr);
}

TEST(YuvaScaleTest, ConstantPlanesStayConstant) {
  Planes src(301, 199, 200, 60, 180, 77);
  src.view.color = wt::YuvColor{wt::YuvMatrix::kBt709, true};
  // libyuv's fixed-point box average may round down by one level; at
  // ratios far beyond any bucket (301 -> 1 px) it drifts further.
  for (uint32_t size : {32u, 64u, 150u, 512u}) {
    SCOPED_TRACE(size);
    wt::YuvaImage dst;
    ASSERT_EQ(dst.Allocate(size, size / 2 + 1, true), WT_OK);
    ASSERT_EQ(wt::ScaleColor(src.view, &dst), WT_OK);
    ASSERT_EQ(wt::ScaleAlpha(src.view, &dst), WT_OK);
    const wt::YuvaView v = dst.view();
    EXPECT_EQ(v.color.matrix, wt::YuvMatrix::kBt709);
    EXPECT_TRUE(v.color.full_range);
    for (uint32_t y = 0; y < v.height; ++y) {
      for (uint32_t x = 0; x < v.width; ++x) {
        ASSERT_NEAR(v.y[y * v.y_stride + x], 200, 1);
        ASSERT_NEAR(v.a[y * v.a_stride + x], 77, 1);
      }
    }
    for (uint32_t y = 0; y < wt::ChromaSize(v.height); ++y) {
      for (uint32_t x = 0; x < wt::ChromaSize(v.width); ++x) {
        ASSERT_NEAR(v.u[y * v.u_stride + x], 60, 1);
        ASSERT_NEAR(v.v[y * v.v_stride + x], 180, 1);
      }
    }
  }
}

TEST(YuvaScaleTest, Arguments) {
  Planes opaque(8, 8, 16, 128, 128, -1);
  wt::YuvaImage with_alpha, without_alpha, unallocated;
  ASSERT_EQ(with_alpha.Allocate(4, 4, true), WT_OK);
  ASSERT_EQ(without_alpha.Allocate(4, 4, false), WT_OK);
  EXPECT_EQ(wt::ScaleAlpha(opaque.view, &with_alpha), WT_ERR_INVALID_ARGUMENT);
  Planes alpha(8, 8, 16, 128, 128, 9);
  EXPECT_EQ(wt::ScaleAlpha(alpha.view, &without_alpha),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ScaleColor(alpha.view, &unallocated), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ScaleColor(wt::YuvaView{}, &with_alpha),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ScaleColor(alpha.view, nullptr), WT_ERR_INVALID_ARGUMENT);
}

}  // namespace
