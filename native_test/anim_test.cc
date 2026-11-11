// Tests for wt_anim through the C ABI (T1.4): every corpus WebM decoded at
// source size against manifest.json, the alpha-less file's alpha, seeking and
// looping, size buckets, the caller's buffer contract, and the errors
// wt_anim_create returns. Pixel comparisons against the reference frames live
// in golden_test.cc.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "anim_util.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "ref_frames.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;
using wt_test::Json;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

// The WebM files that play (expect ok or truncated).
std::vector<const Json*> PlayableWebms() {
  std::vector<const Json*> out;
  for (const Json& f : wt_test::Manifest()["files"].array) {
    if (f["kind"].string != "webm") continue;
    const std::string& expect = f["expect"].string;
    if (expect == "ok" || expect == "truncated") out.push_back(&f);
  }
  return out;
}

using wt_test::Anim;
using wt_test::Next;
using wt_test::OpenAnim;
using wt_test::OpenSource;
using wt_test::OutputSize;
using wt_test::Size;
using wt_test::Source;

// Every colour byte is <= its alpha: a valid premultiplied image.
bool Premultiplied(const Bytes& rgba) {
  for (size_t i = 0; i < rgba.size(); i += 4) {
    const uint8_t a = rgba[i + 3];
    if (rgba[i] > a || rgba[i + 1] > a || rgba[i + 2] > a) return false;
  }
  return true;
}

bool AllOpaque(const Bytes& rgba) {
  for (size_t i = 3; i < rgba.size(); i += 4) {
    if (rgba[i] != 255) return false;
  }
  return true;
}

std::vector<Bytes> DecodeAll(const Anim& anim) {
  std::vector<Bytes> frames;
  Bytes pixels;
  while (Next(anim, &pixels) == WT_OK) frames.push_back(pixels);
  return frames;
}

// --- Corpus at source size -------------------------------------------------

TEST(AnimCorpusTest, EveryWebmMatchesManifest) {
  const std::vector<const Json*> files = PlayableWebms();
  ASSERT_GE(files.size(), 8u);
  for (const Json* file : files) {
    const std::string& name = (*file)["name"].string;
    SCOPED_TRACE(name);
    Anim anim;
    ASSERT_EQ(OpenAnim(name, 0, 0, &anim), WT_OK);

    wt_info info;
    uint32_t w = 0, h = 0;
    ASSERT_EQ(wt_anim_get_info(anim.p, &info, &w, &h), WT_OK);
    const uint32_t frame_count =
        static_cast<uint32_t>((*file)["frame_count"].Int());
    EXPECT_EQ(info.format, WT_FORMAT_WEBM);
    EXPECT_EQ(info.codec,
              (*file)["codec"].string == "vp8" ? WT_CODEC_VP8 : WT_CODEC_VP9);
    EXPECT_EQ(info.width, (*file)["width"].Int());
    EXPECT_EQ(info.height, (*file)["height"].Int());
    EXPECT_EQ(w, info.width);
    EXPECT_EQ(h, info.height);
    EXPECT_EQ(info.frame_count, frame_count);
    EXPECT_EQ(info.has_alpha, (*file)["alpha"].boolean);
    EXPECT_EQ(info.has_audio, (*file)["has_audio"].boolean);
    EXPECT_EQ(info.truncated, (*file)["expect"].string == "truncated");

    const std::vector<int64_t> pts = (*file)["pts_ms"].Ints();
    const std::vector<int64_t> durations = (*file)["durations_ms"].Ints();
    ASSERT_EQ(pts.size(), frame_count);
    ASSERT_EQ(durations.size(), frame_count);

    Bytes pixels;
    for (uint32_t i = 0; i < frame_count; ++i) {
      SCOPED_TRACE("frame " + std::to_string(i));
      wt_frame_info frame;
      ASSERT_EQ(Next(anim, &pixels, WT_PIXEL_FORMAT_RGBA8888, &frame), WT_OK);
      EXPECT_EQ(frame.index, i);
      EXPECT_EQ(frame.pts_ms, pts[i]);
      // The truncated file's last complete frame loses its successor, so its
      // duration falls back to the track default.
      if (i + 1 < frame_count || !info.truncated) {
        EXPECT_EQ(frame.duration_ms, durations[i]);
      }
      EXPECT_TRUE(Premultiplied(pixels));
      if (!info.has_alpha) {
        EXPECT_TRUE(AllOpaque(pixels));
      }
    }
    wt_frame_info frame;
    EXPECT_EQ(Next(anim, &pixels, WT_PIXEL_FORMAT_RGBA8888, &frame), WT_END);
    EXPECT_EQ(frame.index, 0u) << "zeroed on WT_END";
    EXPECT_EQ(Next(anim, &pixels), WT_END) << "stays at the end";
  }
}

// The T1.4 acceptance check: a file without an alpha track is opaque.
TEST(AnimCorpusTest, AlphaLessFileIsOpaque) {
  for (wt_pixel_format format :
       {WT_PIXEL_FORMAT_RGBA8888, WT_PIXEL_FORMAT_BGRA8888}) {
    for (uint32_t bucket : {0u, 64u, 96u, 192u}) {
      Anim anim;
      ASSERT_EQ(OpenAnim("vp9_opaque_256.webm", bucket, bucket, &anim), WT_OK);
      Bytes pixels;
      int frames = 0;
      while (Next(anim, &pixels, format) == WT_OK) {
        ASSERT_TRUE(AllOpaque(pixels)) << "frame " << frames;
        ++frames;
      }
      EXPECT_EQ(frames, 60);
    }
  }
}

TEST(AnimCorpusTest, BgraIsRgbaWithRedAndBlueSwapped) {
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm",
                           "vp9_opaque_256.webm", "vp9_odd_301x199.webm"}) {
    SCOPED_TRACE(name);
    Anim rgba_anim, bgra_anim;
    ASSERT_EQ(OpenAnim(name, 0, 0, &rgba_anim), WT_OK);
    ASSERT_EQ(OpenAnim(name, 0, 0, &bgra_anim), WT_OK);
    Bytes rgba, bgra;
    for (int i = 0; i < 5; ++i) {
      ASSERT_EQ(Next(rgba_anim, &rgba, WT_PIXEL_FORMAT_RGBA8888), WT_OK);
      ASSERT_EQ(Next(bgra_anim, &bgra, WT_PIXEL_FORMAT_BGRA8888), WT_OK);
      for (size_t p = 0; p < bgra.size(); p += 4)
        std::swap(bgra[p], bgra[p + 2]);
      ASSERT_EQ(rgba, bgra) << "frame " << i;
    }
  }
}

TEST(AnimCorpusTest, TruncatedFilePlaysItsCompleteFrames) {
  Anim anim;
  ASSERT_EQ(OpenAnim("truncated.webm", 0, 0, &anim), WT_OK);
  EXPECT_EQ(DecodeAll(anim).size(), 60u);
}

// --- Seeking and looping ---------------------------------------------------

TEST(AnimSeekTest, LoopReplaysIdenticalFrames) {
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm",
                           "vp9_opaque_256.webm", "vp9_single_frame.webm"}) {
    SCOPED_TRACE(name);
    Anim anim;
    ASSERT_EQ(OpenAnim(name, 0, 0, &anim), WT_OK);
    const std::vector<Bytes> first = DecodeAll(anim);
    ASSERT_FALSE(first.empty());
    ASSERT_EQ(wt_anim_seek(anim.p, 0), WT_OK);
    const std::vector<Bytes> second = DecodeAll(anim);
    ASSERT_EQ(second.size(), first.size());
    for (size_t i = 0; i < first.size(); ++i) {
      ASSERT_EQ(first[i], second[i]) << "frame " << i;
    }
  }
}

TEST(AnimSeekTest, SeekMatchesSequentialDecode) {
  struct Case {
    const char* name;
    std::vector<uint32_t> targets;  // In this order, on one animation.
  };
  const Case cases[] = {
      // Keyframes at 0 and 45.
      {"vp9_alpha_512.webm", {50, 45, 44, 89, 0, 1, 46, 47, 20}},
      // Keyframes at 0 and 30.
      {"vp8_alpha_256.webm", {30, 29, 59, 31, 2, 0}},
      // Keyframe at 0 only, with alt-ref superframes.
      {"vp9_opaque_256.webm", {59, 10, 11, 5, 0}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.name);
    Anim sequential, seeking;
    ASSERT_EQ(OpenAnim(c.name, 0, 0, &sequential), WT_OK);
    ASSERT_EQ(OpenAnim(c.name, 0, 0, &seeking), WT_OK);
    const std::vector<Bytes> expected = DecodeAll(sequential);
    for (uint32_t target : c.targets) {
      SCOPED_TRACE("seek " + std::to_string(target));
      ASSERT_EQ(wt_anim_seek(seeking.p, target), WT_OK);
      Bytes pixels;
      wt_frame_info frame;
      ASSERT_EQ(Next(seeking, &pixels, WT_PIXEL_FORMAT_RGBA8888, &frame),
                WT_OK);
      EXPECT_EQ(frame.index, target);
      EXPECT_EQ(pixels, expected[target]);
    }
  }
}

TEST(AnimSeekTest, OutOfRange) {
  Anim anim;
  ASSERT_EQ(OpenAnim("vp9_alpha_vfr.webm", 0, 0, &anim), WT_OK);
  EXPECT_EQ(wt_anim_seek(anim.p, 32), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_anim_seek(anim.p, UINT32_MAX), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_anim_seek(anim.p, 31), WT_OK);
  Bytes pixels;
  EXPECT_EQ(Next(anim, &pixels), WT_OK);
  EXPECT_EQ(Next(anim, &pixels), WT_END);
}

// --- Size buckets ----------------------------------------------------------

TEST(AnimScaleTest, EveryBucketDecodes) {
  const uint32_t kBuckets[] = {64, 96, 128, 192, 256, 384, 512};
  for (const Json* file : PlayableWebms()) {
    const std::string& name = (*file)["name"].string;
    const bool alpha = (*file)["alpha"].boolean;
    for (uint32_t bucket : kBuckets) {
      SCOPED_TRACE(name + " @ " + std::to_string(bucket));
      Anim anim;
      ASSERT_EQ(OpenAnim(name, bucket, bucket, &anim), WT_OK);
      const Size size = OutputSize(anim);
      EXPECT_EQ(size.w, bucket);
      EXPECT_EQ(size.h, bucket);
      Bytes pixels;
      // A few frames per bucket keep this test quick under ASan.
      for (int i = 0; i < 3; ++i) {
        const wt_status s = Next(anim, &pixels);
        if (s == WT_END) break;
        ASSERT_EQ(s, WT_OK);
        EXPECT_TRUE(Premultiplied(pixels));
        if (!alpha) {
          EXPECT_TRUE(AllOpaque(pixels));
        }
      }
    }
  }
}

TEST(AnimScaleTest, NonSquareAndOddBuckets) {
  for (Size size : {Size{150, 99}, Size{192, 128}, Size{1, 1}, Size{7, 301},
                    Size{1024, 1024}}) {
    SCOPED_TRACE(std::to_string(size.w) + "x" + std::to_string(size.h));
    Anim anim;
    ASSERT_EQ(OpenAnim("vp9_odd_301x199.webm", size.w, size.h, &anim), WT_OK);
    Bytes pixels;
    ASSERT_EQ(Next(anim, &pixels), WT_OK);
    EXPECT_EQ(pixels.size(), size_t{size.w} * size.h * 4);
    EXPECT_TRUE(Premultiplied(pixels));
  }
}

// --- The caller's buffer ---------------------------------------------------

TEST(AnimBufferTest, StrideLeavesPaddingUntouched) {
  Anim tight, padded;
  ASSERT_EQ(OpenAnim("vp9_odd_301x199.webm", 0, 0, &tight), WT_OK);
  ASSERT_EQ(OpenAnim("vp9_odd_301x199.webm", 0, 0, &padded), WT_OK);
  const uint32_t w = 301, h = 199, row = w * 4, stride = row + 52;
  // The minimum size: the last row has no padding.
  Bytes buffer(size_t{stride} * (h - 1) + row, 0xCD);
  ASSERT_EQ(wt_anim_next_frame(padded.p, buffer.data(), buffer.size(), stride,
                               WT_PIXEL_FORMAT_BGRA8888, nullptr),
            WT_OK);
  Bytes expected;
  ASSERT_EQ(Next(tight, &expected, WT_PIXEL_FORMAT_BGRA8888), WT_OK);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* line = buffer.data() + size_t{y} * stride;
    ASSERT_TRUE(std::equal(line, line + row, expected.data() + size_t{y} * row))
        << "row " << y;
    if (y + 1 < h) {
      ASSERT_TRUE(std::all_of(line + row, line + stride,
                              [](uint8_t b) { return b == 0xCD; }))
          << "padding of row " << y;
    }
  }
}

TEST(AnimBufferTest, BadBuffersAreRejectedWithoutAdvancing) {
  Anim anim;
  ASSERT_EQ(OpenAnim("vp9_alpha_vfr.webm", 64, 32, &anim), WT_OK);
  const uint32_t row = 64 * 4;
  Bytes buffer(size_t{row} * 32 + 64);
  wt_frame_info frame;
  // Stride shorter than a row.
  EXPECT_EQ(wt_anim_next_frame(anim.p, buffer.data(), buffer.size(), row - 1,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
  // One byte short.
  EXPECT_EQ(wt_anim_next_frame(anim.p, buffer.data(), size_t{row} * 32 - 1, row,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
  // A stride that needs more than the buffer holds.
  EXPECT_EQ(wt_anim_next_frame(anim.p, buffer.data(), buffer.size(), row + 4,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
  // A stride beyond libyuv's int, and a stride whose total overflows 32 bits.
  EXPECT_EQ(wt_anim_next_frame(anim.p, buffer.data(), SIZE_MAX, 0x80000000u,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_anim_next_frame(anim.p, buffer.data(), buffer.size(),
                               0x7FFFFFFFu, WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(
      wt_anim_next_frame(anim.p, buffer.data(), buffer.size(), row, 0, &frame),
      WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(
      wt_anim_next_frame(anim.p, buffer.data(), buffer.size(), row, 3, &frame),
      WT_ERR_INVALID_ARGUMENT);
  // None of those advanced the cursor.
  ASSERT_EQ(wt_anim_next_frame(anim.p, buffer.data(), size_t{row} * 32, row,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_OK);
  EXPECT_EQ(frame.index, 0u);
}

// --- Creation --------------------------------------------------------------

TEST(AnimCreateTest, Errors) {
  Anim anim;
  EXPECT_EQ(OpenAnim("garbage.bin", 0, 0, &anim), WT_ERR_INVALID_DATA);
  EXPECT_EQ(OpenAnim("vp9_2048.webm", 0, 0, &anim), WT_ERR_LIMIT);
  // GIF arrives in T5.2.
  EXPECT_EQ(OpenAnim("anim_disposal.gif", 0, 0, &anim), WT_ERR_UNSUPPORTED);
  EXPECT_EQ(anim.p, nullptr);

  // Output sizes: both zero or both in 1..max_dimension.
  EXPECT_EQ(OpenAnim("vp9_alpha_vfr.webm", 0, 64, &anim),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(OpenAnim("vp9_alpha_vfr.webm", 64, 0, &anim),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(OpenAnim("vp9_alpha_vfr.webm", 1025, 64, &anim),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(OpenAnim("vp9_alpha_vfr.webm", 64, 1025, &anim),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(anim.p, nullptr);
  ASSERT_EQ(OpenAnim("vp9_alpha_vfr.webm", 1024, 1024, &anim), WT_OK);
}

TEST(AnimCreateTest, OutputSizeFollowsTheSourceLimits) {
  wt_limits limits;
  wt_limits_init_default(&limits);
  limits.max_dimension = 200;
  Source source;
  ASSERT_EQ(OpenSource("vp9_alpha_vfr.webm", &source, &limits), WT_OK);
  Anim anim;
  EXPECT_EQ(wt_anim_create(source.p, 256, 256, &anim.p),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_anim_create(source.p, 200, 200, &anim.p), WT_OK);

  // A 256 px source is over this limit, whatever the output size.
  Source big;
  ASSERT_EQ(OpenSource("vp9_opaque_256.webm", &big, &limits), WT_OK);
  Anim small;
  EXPECT_EQ(wt_anim_create(big.p, 64, 64, &small.p), WT_ERR_LIMIT);
}

TEST(AnimCreateTest, AnimationsOutliveTheirSource) {
  std::ifstream in(kAssets / "vp9_alpha_vfr.webm", std::ios::binary);
  Bytes bytes((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
  wt_source* source = nullptr;
  ASSERT_EQ(
      wt_source_create_memory(bytes.data(), bytes.size(), nullptr, &source),
      WT_OK);
  Anim a, b;
  ASSERT_EQ(wt_anim_create(source, 0, 0, &a.p), WT_OK);
  ASSERT_EQ(wt_anim_create(source, 64, 64, &b.p), WT_OK);
  wt_source_destroy(source);  // Both animations still hold the bytes.
  std::fill(bytes.begin(), bytes.end(), 0);
  EXPECT_EQ(DecodeAll(a).size(), 32u);
  wt_anim_destroy(a.p);
  a.p = nullptr;
  EXPECT_EQ(DecodeAll(b).size(), 32u);  // ASan flags any use after free.
}

TEST(AnimCreateTest, GetInfoOutputsAreOptional) {
  Anim anim;
  ASSERT_EQ(OpenAnim("vp9_with_audio.webm", 96, 96, &anim), WT_OK);
  EXPECT_EQ(wt_anim_get_info(anim.p, nullptr, nullptr, nullptr), WT_OK);
  wt_info info;
  ASSERT_EQ(wt_anim_get_info(anim.p, &info, nullptr, nullptr), WT_OK);
  EXPECT_EQ(info.width, 128u) << "info has the source size";
  EXPECT_EQ(info.frame_count, 30u);
  EXPECT_EQ(info.duration_ms, 1000u);
  EXPECT_NEAR(info.fps, 30.0, 0.01);
  EXPECT_TRUE(info.has_audio);
}

}  // namespace
