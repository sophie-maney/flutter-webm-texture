// Animated WebP (T5.1): the probe and demux against manifest.json, the
// duration and still-image policies, truncated and hostile files, limits,
// and wt::Anim playing a WebP (seek, skip, frame cache). Pixel accuracy
// against the reference frames is in golden_test.cc.
#include "demux/webp.h"

#include <unistd.h>

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
#include "demux/sniff.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using wt_test::Bytes;

const fs::path kAssets = WT_TEST_ASSETS_DIR;
// Small hand-made files shared with the fuzzer's seeds (fuzz/corpus/webp).
const fs::path kSeeds = kAssets / ".." / "fuzz" / "corpus" / "webp";

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
  wt_anim* anim = nullptr;
  ~Owned() {
    wt_anim_destroy(anim);
    wt_source_destroy(source);
  }
};

wt_status Probe(const Bytes& b, wt_info* info,
                const wt_limits* limits = nullptr) {
  Owned o;
  EXPECT_EQ(wt_source_create_memory(b.data(), b.size(), limits, &o.source),
            WT_OK);
  return wt_probe(o.source, info);
}

const wt_test::Json& Entry() {
  for (const wt_test::Json& f : wt_test::Manifest()["files"].array) {
    if (f["name"].string == "anim.webp") return f;
  }
  ADD_FAILURE() << "anim.webp is not in the manifest";
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

// --- Probe and demux --------------------------------------------------------

TEST(WebpProbeTest, MatchesManifest) {
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
  wt_info info;
  std::memset(&info, 0xAB, sizeof(info));
  ASSERT_EQ(Probe(bytes, &info), WT_OK);
  const wt_test::Json& e = Entry();
  EXPECT_EQ(info.format, WT_FORMAT_WEBP);
  EXPECT_EQ(info.codec, WT_CODEC_WEBP);
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

TEST(WebpProbeTest, PlaybackTimingMatchesManifest) {
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
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

TEST(WebpProbeTest, DurationPolicy) {
  // Browsers play 10 ms and under (and the 0 of stills) at 100 ms.
  EXPECT_EQ(wt::WebpFrameDurationMs(-5), 100u);
  EXPECT_EQ(wt::WebpFrameDurationMs(0), 100u);
  EXPECT_EQ(wt::WebpFrameDurationMs(1), 100u);
  EXPECT_EQ(wt::WebpFrameDurationMs(10), 100u);
  EXPECT_EQ(wt::WebpFrameDurationMs(11), 11u);
  EXPECT_EQ(wt::WebpFrameDurationMs(50), 50u);
  EXPECT_EQ(wt::WebpFrameDurationMs(16777215), 16777215u);
}

// anim_zero_duration.webp: two 12x8 frames muxed with durations 0 and 40.
TEST(WebpProbeTest, ZeroAndShortDurationsFollowThePolicy) {
  const Bytes bytes = ReadFile(kSeeds / "anim_zero_duration.webp");
  ASSERT_FALSE(bytes.empty());
  wt_info info;
  ASSERT_EQ(Probe(bytes, &info), WT_OK);
  EXPECT_EQ(info.frame_count, 2u);
  EXPECT_EQ(info.duration_ms, 140u);
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim), WT_OK);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.infos.size(), 2u);
  EXPECT_EQ(p.infos[0].duration_ms, 100u);
  EXPECT_EQ(p.infos[0].pts_ms, 0);
  EXPECT_EQ(p.infos[1].duration_ms, 40u);
  EXPECT_EQ(p.infos[1].pts_ms, 100);
}

// A still WebP is a one-frame animation that shows for 100 ms.
TEST(WebpProbeTest, StillImagesAreOneFrame) {
  struct Case {
    const char* file;
    uint32_t w, h;
    bool alpha;
  };
  for (const Case& c : {Case{"still_lossless_alpha.webp", 12, 8, true},
                        Case{"still_lossy.webp", 16, 16, false}}) {
    SCOPED_TRACE(c.file);
    const Bytes bytes = ReadFile(kSeeds / c.file);
    ASSERT_FALSE(bytes.empty());
    wt_info info;
    ASSERT_EQ(Probe(bytes, &info), WT_OK);
    EXPECT_EQ(info.format, WT_FORMAT_WEBP);
    EXPECT_EQ(info.width, c.w);
    EXPECT_EQ(info.height, c.h);
    EXPECT_EQ(info.frame_count, 1u);
    EXPECT_EQ(info.duration_ms, 100u);
    EXPECT_EQ(info.has_alpha, c.alpha);
    std::unique_ptr<wt::Anim> anim;
    ASSERT_EQ(Create(bytes, 0, 0, &anim), WT_OK);
    const Played p = Play(anim.get());
    ASSERT_EQ(p.infos.size(), 1u);
    EXPECT_EQ(p.statuses[0], WT_OK);
    EXPECT_EQ(p.infos[0].duration_ms, 100u);
    if (!c.alpha) {
      for (size_t i = 3; i < p.pixels[0].size(); i += 4) {
        ASSERT_EQ(p.pixels[0][i], 255);
      }
    }
  }
}

// --- Truncated and damaged input
// -----------------------------------------------

// Every prefix of anim.webp either fails cleanly or plays the complete frames
// before the cut, with the same durations as the whole file.
TEST(WebpTruncationTest, EveryPrefixFailsOrPlaysItsCompleteFrames) {
  const Bytes whole = ReadFile(kAssets / "anim.webp");
  std::vector<uint32_t> durations;
  {
    std::unique_ptr<wt::Anim> a;
    ASSERT_EQ(Create(whole, 0, 0, &a), WT_OK);
    for (uint32_t i = 0; i < a->info().frame_count; ++i) {
      durations.push_back(a->frame_duration_ms(i));
    }
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
    ASSERT_LT(info.frame_count, durations.size());
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
    }
  }
  EXPECT_GT(playable, 100u) << "most cuts leave complete frames";
  EXPECT_EQ(previous, durations.size() - 1);
}

TEST(WebpTruncationTest, CutBeforeTheFirstFrameIsTruncated) {
  const Bytes whole = ReadFile(kAssets / "anim.webp");
  wt_info info;
  const Bytes header(whole.begin(), whole.begin() + 20);
  EXPECT_EQ(Probe(header, &info), WT_ERR_TRUNCATED);
  EXPECT_EQ(Probe(Bytes(whole.begin(), whole.begin() + 60), &info),
            WT_ERR_TRUNCATED);
}

TEST(WebpTruncationTest, TruncatedPlaybackMatchesTheWholeFile) {
  const Bytes whole = ReadFile(kAssets / "anim.webp");
  std::unique_ptr<wt::Anim> full;
  ASSERT_EQ(Create(whole, 0, 0, &full), WT_OK);
  const Played ref = Play(full.get());
  const Bytes cut(whole.begin(), whole.begin() + 9000);
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(cut, 0, 0, &anim), WT_OK);
  EXPECT_TRUE(anim->info().truncated);
  const Played p = Play(anim.get());
  ASSERT_GE(p.pixels.size(), 2u);
  ASSERT_LT(p.pixels.size(), ref.pixels.size());
  for (size_t i = 0; i < p.pixels.size(); ++i) {
    EXPECT_TRUE(p.pixels[i] == ref.pixels[i]) << i;
  }
}

TEST(WebpErrorTest, GarbageAfterTheMagicIsInvalid) {
  Bytes b = {'R', 'I', 'F', 'F', 0x20, 0, 0, 0, 'W', 'E', 'B', 'P'};
  for (int i = 0; i < 40; ++i) b.push_back(static_cast<uint8_t>(i * 37));
  wt_info info;
  const wt_status s = Probe(b, &info);
  EXPECT_TRUE(s == WT_ERR_INVALID_DATA || s == WT_ERR_TRUNCATED)
      << wt_status_string(s);
  EXPECT_EQ(info.width, 0u);
}

TEST(WebpErrorTest, FlippedBytesNeverCrash) {
  const Bytes whole = ReadFile(kAssets / "anim.webp");
  for (size_t i = 0; i < whole.size(); i += 13) {
    Bytes b = whole;
    b[i] ^= 0xFF;
    std::unique_ptr<wt::Anim> anim;
    if (Create(b, 24, 24, &anim) != WT_OK) continue;
    const Played p = Play(anim.get());
    for (wt_status s : p.statuses) {
      EXPECT_TRUE(s == WT_OK || s == WT_ERR_DECODE || s == WT_ERR_LIMIT)
          << i << ": " << wt_status_string(s);
    }
  }
}

// --- Limits
// -------------------------------------------------------------------

TEST(WebpLimitTest, EachLimitAtAndPastItsBound) {
  const Bytes bytes =
      ReadFile(kAssets / "anim.webp");  // 128x128, 16 f, 1650 ms
  wt_info info;
  wt_limits l = DefaultLimits();
  ASSERT_EQ(Probe(bytes, &info, &l), WT_OK);
  const uint64_t frame_pixels = 128 * 128;

  l = DefaultLimits();
  l.max_dimension = 128;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_dimension = 127;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);
  EXPECT_EQ(info.width, 0u);

  l = DefaultLimits();
  l.max_frames = 16;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_frames = 15;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  l = DefaultLimits();
  l.max_total_pixels = 16 * frame_pixels;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_total_pixels = 16 * frame_pixels - 1;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  l = DefaultLimits();
  l.max_duration_ms = 1650;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_OK);
  l.max_duration_ms = 1649;
  EXPECT_EQ(Probe(bytes, &info, &l), WT_ERR_LIMIT);

  // wt_anim_create fails the same way.
  l = DefaultLimits();
  l.max_frames = 15;
  std::unique_ptr<wt::Anim> anim;
  EXPECT_EQ(Create(bytes, 0, 0, &anim, &l), WT_ERR_LIMIT);
  EXPECT_EQ(anim, nullptr);

  // Zero is invalid, not unlimited.
  l = DefaultLimits();
  l.max_frames = 0;
  wt::WebpIndex index;
  EXPECT_EQ(wt::DemuxWebp(View(bytes), l, &index), WT_ERR_INVALID_ARGUMENT);
}

// The decode budget counts every canvas decoded since the last seek, replay
// after a backward seek included: one full pass fits exactly, one frame more
// doesn't.
TEST(WebpLimitTest, DecodeBudgetCountsSeekReplay) {
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
  wt_limits l = DefaultLimits();
  l.max_total_pixels = 16 * 128 * 128;  // Exactly one full pass.
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 0, 0, &anim, &l), WT_OK);
  Bytes px(128 * 128 * 4);
  wt_frame_info info;
  auto next = [&] {
    return anim->NextFrame(px.data(), px.size(), 128 * 4,
                           WT_PIXEL_FORMAT_RGBA8888, &info);
  };
  // Frame 15 after a seek is 16 decodes: the budget exactly. A seek starts a
  // new pass, so doing it again works.
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(anim->Seek(15), WT_OK);
    ASSERT_EQ(next(), WT_OK);
    EXPECT_EQ(info.index, 15u);
  }
  // Seeking to 0 and playing the whole animation fits the budget too, but
  // an extra decode in the same pass (a backwards seek would replay) doesn't.
  ASSERT_EQ(anim->Seek(3), WT_OK);
  ASSERT_EQ(next(), WT_OK);  // 4 decodes.
  ASSERT_EQ(anim->Seek(2), WT_OK);
  ASSERT_EQ(next(), WT_OK);  // Reset + 3 decodes, in a new pass.
}

// --- Playback behaviour
// -----------------------------------------------------------

TEST(WebpPlaybackTest, SkipAndSeekMatchSequentialPlayback) {
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
  std::unique_ptr<wt::Anim> a;
  ASSERT_EQ(Create(bytes, 0, 0, &a), WT_OK);
  const Played ref = Play(a.get());
  ASSERT_EQ(ref.pixels.size(), 16u);

  std::unique_ptr<wt::Anim> b;
  ASSERT_EQ(Create(bytes, 0, 0, &b), WT_OK);
  const size_t stride = 128 * 4;
  Bytes px(stride * 128);
  wt_frame_info info;
  // Skip frames 0..5 (the compositing state still advances), then read 6.
  for (int i = 0; i < 6; ++i) ASSERT_EQ(b->SkipFrame(&info), WT_OK);
  ASSERT_EQ(b->NextFrame(px.data(), px.size(), stride, WT_PIXEL_FORMAT_RGBA8888,
                         &info),
            WT_OK);
  EXPECT_EQ(info.index, 6u);
  EXPECT_TRUE(px == ref.pixels[6]);
  // Back to the start and to the end again.
  for (uint32_t i : {0u, 15u, 4u, 4u, 5u}) {
    ASSERT_EQ(b->Seek(i), WT_OK);
    ASSERT_EQ(b->NextFrame(px.data(), px.size(), stride,
                           WT_PIXEL_FORMAT_RGBA8888, &info),
              WT_OK);
    EXPECT_EQ(info.index, i);
    EXPECT_TRUE(px == ref.pixels[i]) << i;
  }
  EXPECT_EQ(b->Seek(16), WT_ERR_INVALID_ARGUMENT);
}

TEST(WebpPlaybackTest, ScaledOutputIsOpaqueWithoutAlpha) {
  const Bytes bytes = ReadFile(kSeeds / "still_lossy.webp");
  std::unique_ptr<wt::Anim> anim;
  ASSERT_EQ(Create(bytes, 5, 7, &anim), WT_OK);
  EXPECT_EQ(anim->width(), 5u);
  EXPECT_EQ(anim->height(), 7u);
  const Played p = Play(anim.get());
  ASSERT_EQ(p.pixels.size(), 1u);
  for (size_t i = 3; i < p.pixels[0].size(); i += 4) {
    ASSERT_EQ(p.pixels[0][i], 255);
  }
}

// --- Frame cache
// ----------------------------------------------------------------------

class WebpCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           ("wt_webp_cache_" + std::to_string(getpid()));
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

TEST_F(WebpCacheTest, WritesOnTheFirstPassThenPlaysIdentically) {
  using Mode = wt::Anim::CacheMode;
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
  for (uint32_t size : {0u, 48u}) {
    SCOPED_TRACE(size);
    const std::string path = File(size == 0 ? "full.wsc" : "small.wsc");
    std::unique_ptr<wt::Anim> plain;
    ASSERT_EQ(Create(bytes, size, size, &plain), WT_OK);
    EXPECT_EQ(plain->cache_mode(), Mode::kNone);
    const Played ref = Play(plain.get());
    ASSERT_EQ(ref.pixels.size(), 16u);

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

// Reading the cache gives the same frames as decoding, after a seek too; and a
// cache made from a different file is rejected.
TEST_F(WebpCacheTest, ForeignCacheIsReplaced) {
  using Mode = wt::Anim::CacheMode;
  const Bytes bytes = ReadFile(kAssets / "anim.webp");
  const std::string path = File("c.wsc");
  std::unique_ptr<wt::Anim> a;
  ASSERT_EQ(Create(bytes, 0, 0, &a, nullptr, path.c_str()), WT_OK);
  Play(a.get());
  ASSERT_EQ(a->cache_mode(), Mode::kReading);
  Bytes other = bytes;
  other[other.size() / 2] ^= 1;  // Same size, other content.
  std::unique_ptr<wt::Anim> b;
  // The flip lands in pixel data, which probing doesn't read.
  ASSERT_EQ(Create(other, 0, 0, &b, nullptr, path.c_str()), WT_OK);
  EXPECT_NE(b->cache_mode(), Mode::kReading);
}

}  // namespace
