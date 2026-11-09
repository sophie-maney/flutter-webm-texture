// Golden frame tests (T1.6): frames decoded through the C ABI against the
// reference frames in test_assets/ref (ADR 0001).
//
// - Source size: every sampled frame, decoded sequentially and by a cold
//   seek, must reach PSNR >= 45 dB on colour and match the reference alpha
//   exactly.
// - Buckets: scaled output is spot-checked against an area-averaged
//   reference with looser tolerances. The core scales straight (not
//   premultiplied) channels and the references are premultiplied, so soft
//   edges differ a little; the thresholds catch a wrong filter, a shifted
//   or flipped image and a swapped channel, not rounding.
//
// WebP and GIF references are not checked here: wt_anim_create returns
// WT_ERR_UNSUPPORTED for them until Phase 5.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "anim_util.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "ref_frames.h"
#include "wt_api.h"

namespace {

using wt_test::Anim;
using wt_test::Bytes;
using wt_test::Json;
using wt_test::Next;
using wt_test::OpenAnim;

constexpr unsigned kRgb = 0x7;
constexpr unsigned kAlpha = 0x8;

// Source size.
constexpr double kMinPsnr = 45.0;
// Scaled buckets (see the file comment). Measured minimums over the corpus
// are 36.3 dB (colour) and 33.2 dB (alpha), both at non-integer ratios to
// tiny buckets; the thresholds leave about 2-3 dB of margin.
constexpr double kMinBucketRgbPsnr = 34.0;
constexpr double kMinBucketAlphaPsnr = 30.0;

struct Golden {
  std::string name;
  uint32_t width = 0, height = 0;
  bool alpha = false;
  std::vector<uint32_t> frames;  // Indices that have a reference frame.
};

// The playable WebMs that have reference frames.
std::vector<Golden> GoldenWebms() {
  std::vector<Golden> out;
  for (const Json& f : wt_test::Manifest()["files"].array) {
    if (f["kind"].string != "webm") continue;
    Golden g;
    for (int64_t i : f["ref_frames"].Ints()) {
      g.frames.push_back(static_cast<uint32_t>(i));
    }
    if (g.frames.empty()) continue;
    g.name = f["name"].string;
    g.width = static_cast<uint32_t>(f["width"].Int());
    g.height = static_cast<uint32_t>(f["height"].Int());
    g.alpha = f["alpha"].boolean;
    out.push_back(std::move(g));
  }
  return out;
}

bool Has(const Golden& g, uint32_t frame) {
  return std::find(g.frames.begin(), g.frames.end(), frame) != g.frames.end();
}

// Colour within kMinPsnr and alpha identical. Alpha comes straight from the
// alpha stream, so the only error it can carry is a decode bug.
void ExpectGolden(const Bytes& pixels, const Bytes& ref) {
  ASSERT_EQ(pixels.size(), ref.size());
  EXPECT_EQ(wt_test::MaxError(pixels, ref, kAlpha), 0) << "alpha differs";
  EXPECT_GE(wt_test::Psnr(pixels, ref, kRgb), kMinPsnr);
}

TEST(GoldenTest, CorpusHasReferences) {
  const std::vector<Golden> files = GoldenWebms();
  // Guards against the manifest drifting away from what the tests expect.
  EXPECT_GE(files.size(), 7u);
  size_t refs = 0;
  for (const Golden& g : files) refs += g.frames.size();
  EXPECT_GE(refs, 20u);
}

// Plays each clip from the start and checks every sampled frame. This also
// covers inter-frame drift: the sampled set includes the frame before each
// keyframe.
TEST(GoldenTest, SequentialDecodeMatchesReferencesAtSourceSize) {
  for (const Golden& g : GoldenWebms()) {
    SCOPED_TRACE(g.name);
    Anim anim;
    ASSERT_EQ(OpenAnim(g.name, 0, 0, &anim), WT_OK);
    const uint32_t last = *std::max_element(g.frames.begin(), g.frames.end());
    Bytes pixels, ref;
    for (uint32_t i = 0; i <= last; ++i) {
      ASSERT_EQ(Next(anim, &pixels), WT_OK) << "frame " << i;
      if (!Has(g, i)) continue;
      SCOPED_TRACE("frame " + std::to_string(i));
      ASSERT_TRUE(wt_test::LoadRefFrame(g.name, static_cast<int>(i), g.width,
                                        g.height, &ref));
      ExpectGolden(pixels, ref);
    }
  }
}

// Jumps straight to each sampled frame on a fresh animation. A seek restarts
// from the preceding keyframe, so this catches state (reference frames, the
// kept alpha) that sequential playback would hide.
TEST(GoldenTest, SeekedFramesMatchReferencesAtSourceSize) {
  for (const Golden& g : GoldenWebms()) {
    SCOPED_TRACE(g.name);
    Anim anim;
    ASSERT_EQ(OpenAnim(g.name, 0, 0, &anim), WT_OK);
    Bytes pixels, ref;
    // Descending, then ascending, so both backward and forward seeks run.
    std::vector<uint32_t> order(g.frames.rbegin(), g.frames.rend());
    order.insert(order.end(), g.frames.begin(), g.frames.end());
    for (uint32_t i : order) {
      SCOPED_TRACE("frame " + std::to_string(i));
      ASSERT_EQ(wt_anim_seek(anim.p, i), WT_OK);
      wt_frame_info info;
      ASSERT_EQ(Next(anim, &pixels, WT_PIXEL_FORMAT_RGBA8888, &info), WT_OK);
      EXPECT_EQ(info.index, i);
      ASSERT_TRUE(wt_test::LoadRefFrame(g.name, static_cast<int>(i), g.width,
                                        g.height, &ref));
      ExpectGolden(pixels, ref);
    }
  }
}

// BGRA output is the RGBA reference with red and blue swapped.
TEST(GoldenTest, BgraMatchesSwappedReferences) {
  for (const Golden& g : GoldenWebms()) {
    SCOPED_TRACE(g.name);
    Anim anim;
    ASSERT_EQ(OpenAnim(g.name, 0, 0, &anim), WT_OK);
    // One frame per clip is enough: the format conversion has no state.
    const uint32_t i = g.frames[g.frames.size() / 2];
    ASSERT_EQ(wt_anim_seek(anim.p, i), WT_OK);
    Bytes pixels, ref;
    ASSERT_EQ(Next(anim, &pixels, WT_PIXEL_FORMAT_BGRA8888), WT_OK);
    ASSERT_TRUE(wt_test::LoadRefFrame(g.name, static_cast<int>(i), g.width,
                                      g.height, &ref));
    for (size_t p = 0; p < pixels.size(); p += 4)
      std::swap(pixels[p], pixels[p + 2]);
    ExpectGolden(pixels, ref);
  }
}

struct Bucket {
  uint32_t w, h;
};

// The aspect-preserving bucket for a source, `scale` (0, 1] of its size.
Bucket BucketFor(const Golden& g, double scale) {
  return {std::max<uint32_t>(1, static_cast<uint32_t>(g.width * scale + 0.5)),
          std::max<uint32_t>(1, static_cast<uint32_t>(g.height * scale + 0.5))};
}

// Scaled output against the area-averaged reference, on the first, middle
// and last sampled frame of each clip.
TEST(GoldenTest, ScaledBucketsMatchAreaAveragedReferences) {
  for (const Golden& g : GoldenWebms()) {
    for (double scale : {0.5, 0.375, 0.25}) {
      const Bucket b = BucketFor(g, scale);
      SCOPED_TRACE(g.name + " @ " + std::to_string(b.w) + "x" +
                   std::to_string(b.h));
      Anim anim;
      ASSERT_EQ(OpenAnim(g.name, b.w, b.h, &anim), WT_OK);
      for (uint32_t i :
           {g.frames.front(), g.frames[g.frames.size() / 2], g.frames.back()}) {
        SCOPED_TRACE("frame " + std::to_string(i));
        Bytes ref;
        ASSERT_TRUE(wt_test::LoadRefFrame(g.name, static_cast<int>(i), g.width,
                                          g.height, &ref));
        const Bytes expected =
            wt_test::AreaDownscale(ref, g.width, g.height, b.w, b.h);
        ASSERT_EQ(wt_anim_seek(anim.p, i), WT_OK);
        Bytes pixels;
        ASSERT_EQ(Next(anim, &pixels), WT_OK);
        ASSERT_EQ(pixels.size(), expected.size());
        EXPECT_GE(wt_test::Psnr(pixels, expected, kRgb), kMinBucketRgbPsnr);
        EXPECT_GE(wt_test::Psnr(pixels, expected, kAlpha), kMinBucketAlphaPsnr);
        if (!g.alpha) {
          for (size_t p = 3; p < pixels.size(); p += 4) {
            ASSERT_EQ(pixels[p], 255) << "alpha-less clip must stay opaque";
          }
        }
      }
    }
  }
}

}  // namespace
