// Tests for wt_probe and limit enforcement (T1.5): probe values against
// manifest.json for every corpus file, each wt_limits field at, and just
// past, its bound, and that wt_probe and wt_anim_create agree.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "manifest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using wt_test::Json;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

struct Source {
  wt_source* p = nullptr;
  ~Source() { wt_source_destroy(p); }
};
struct Anim {
  wt_anim* p = nullptr;
  ~Anim() { wt_anim_destroy(p); }
};

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

wt_status Open(const std::string& name, Source* source,
               const wt_limits* limits = nullptr) {
  return wt_source_create_file((kAssets / name).string().c_str(), limits,
                               &source->p);
}

// Probes a corpus file. Source creation itself must succeed.
wt_status Probe(const std::string& name, wt_info* info,
                const wt_limits* limits = nullptr) {
  Source source;
  EXPECT_EQ(Open(name, &source, limits), WT_OK) << name;
  return wt_probe(source.p, info);
}

bool IsZero(const wt_info& info) {
  wt_info zero;
  std::memset(&zero, 0, sizeof(zero));
  return std::memcmp(&info, &zero, sizeof(info)) == 0;
}

int64_t SumMs(const std::vector<int64_t>& v) {
  return std::accumulate(v.begin(), v.end(), int64_t{0});
}

const Json& FileEntry(const std::string& name) {
  for (const Json& f : wt_test::Manifest()["files"].array) {
    if (f["name"].string == name) return f;
  }
  ADD_FAILURE() << "not in manifest: " << name;
  static const Json none;
  return none;
}

// --- Probe values ----------------------------------------------------------

TEST(ProbeCorpusTest, EveryWebmMatchesManifest) {
  int checked = 0;
  for (const Json& file : wt_test::Manifest()["files"].array) {
    if (file["kind"].string != "webm") continue;
    const std::string& expect = file["expect"].string;
    if (expect != "ok" && expect != "truncated") continue;
    const std::string& name = file["name"].string;
    SCOPED_TRACE(name);
    wt_info info;
    std::memset(&info, 0xAB, sizeof(info));
    ASSERT_EQ(Probe(name, &info), WT_OK);
    ++checked;

    const uint32_t frames = static_cast<uint32_t>(file["frame_count"].Int());
    EXPECT_EQ(info.format, WT_FORMAT_WEBM);
    EXPECT_EQ(info.codec,
              file["codec"].string == "vp8" ? WT_CODEC_VP8 : WT_CODEC_VP9);
    EXPECT_EQ(info.width, file["width"].Int());
    EXPECT_EQ(info.height, file["height"].Int());
    EXPECT_EQ(info.frame_count, frames);
    EXPECT_EQ(info.has_alpha, file["alpha"].boolean);
    EXPECT_EQ(info.has_audio, file["has_audio"].boolean);
    EXPECT_EQ(info.truncated, expect == "truncated");

    // The per-frame durations are rounded individually, the total once, so
    // they can differ by up to half a millisecond per frame.
    const std::vector<int64_t> durations = file["durations_ms"].Ints();
    ASSERT_EQ(durations.size(), frames);
    if (!info.truncated) {
      EXPECT_NEAR(static_cast<double>(info.duration_ms),
                  static_cast<double>(SumMs(durations)), frames * 0.5 + 1);
    }
    EXPECT_GT(info.duration_ms, 0u);
    EXPECT_DOUBLE_EQ(info.fps, info.frame_count * 1000.0 / info.duration_ms);
  }
  EXPECT_GE(checked, 8);
}

TEST(ProbeCorpusTest, FpsOfAConstantRateFile) {
  wt_info info;
  ASSERT_EQ(Probe("vp9_alpha_512.webm", &info), WT_OK);
  EXPECT_EQ(info.frame_count, 90u);
  EXPECT_NEAR(info.fps, 30.0, 0.1);
  EXPECT_NEAR(info.duration_ms, 3000, 2);
}

TEST(ProbeCorpusTest, SingleFrame) {
  wt_info info;
  ASSERT_EQ(Probe("vp9_single_frame.webm", &info), WT_OK);
  EXPECT_EQ(info.frame_count, 1u);
  EXPECT_GT(info.duration_ms, 0u);
}

TEST(ProbeCorpusTest, TruncatedFileCountsOnlyCompleteFrames) {
  wt_info info;
  ASSERT_EQ(Probe("truncated.webm", &info), WT_OK);
  EXPECT_TRUE(info.truncated);
  EXPECT_EQ(info.frame_count, FileEntry("truncated.webm")["frame_count"].Int());
  wt_info full;
  ASSERT_EQ(Probe("vp9_alpha_512.webm", &full), WT_OK);
  EXPECT_LT(info.frame_count, full.frame_count);
  EXPECT_LT(info.duration_ms, full.duration_ms);
}

TEST(ProbeCorpusTest, Repeatable) {
  Source source;
  ASSERT_EQ(Open("vp9_alpha_512.webm", &source), WT_OK);
  wt_info a, b;
  ASSERT_EQ(wt_probe(source.p, &a), WT_OK);
  ASSERT_EQ(wt_probe(source.p, &b), WT_OK);
  EXPECT_EQ(std::memcmp(&a, &b, sizeof(a)), 0);
}

TEST(ProbeCorpusTest, ConcurrentProbesOfOneSource) {
  Source source;
  ASSERT_EQ(Open("vp9_alpha_512.webm", &source), WT_OK);
  wt_info expected;
  ASSERT_EQ(wt_probe(source.p, &expected), WT_OK);
  std::vector<std::thread> threads;
  std::vector<wt_status> status(4, WT_ERR_INTERNAL);
  std::vector<wt_info> infos(4);
  for (size_t i = 0; i < 4; ++i) {
    threads.emplace_back([&, i] { status[i] = wt_probe(source.p, &infos[i]); });
  }
  for (std::thread& t : threads) t.join();
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(status[i], WT_OK);
    EXPECT_EQ(std::memcmp(&infos[i], &expected, sizeof(expected)), 0);
  }
}

// --- Errors ----------------------------------------------------------------

TEST(ProbeErrorTest, CorpusErrors) {
  wt_info info;
  std::memset(&info, 0xAB, sizeof(info));
  EXPECT_EQ(Probe("vp9_2048.webm", &info), WT_ERR_LIMIT);
  EXPECT_TRUE(IsZero(info)) << "zeroed on failure";

  std::memset(&info, 0xAB, sizeof(info));
  EXPECT_EQ(Probe("garbage.bin", &info), WT_ERR_INVALID_DATA);
  EXPECT_TRUE(IsZero(info));

  // Sniffed fine, decoded in T5.2.
  std::memset(&info, 0xAB, sizeof(info));
  EXPECT_EQ(Probe("anim_disposal.gif", &info), WT_ERR_UNSUPPORTED);
}

// T1.5 acceptance: vp9_2048.webm is WT_ERR_LIMIT at default limits, and
// the same through wt_anim_create.
TEST(ProbeErrorTest, OversizedFileIsALimitErrorEverywhere) {
  Source source;
  ASSERT_EQ(Open("vp9_2048.webm", &source), WT_OK)
      << "it is small on disk; size is a probe-time limit";
  wt_info info;
  EXPECT_EQ(wt_probe(source.p, &info), WT_ERR_LIMIT);
  Anim anim;
  EXPECT_EQ(wt_anim_create(source.p, 0, 0, &anim.p), WT_ERR_LIMIT);
  EXPECT_EQ(anim.p, nullptr);

  // Raising max_dimension lets the dimension pass; then the pixel budget
  // (2 frames x 2048 x 2048 = 8 Mi px) is well inside the default.
  wt_limits loose = DefaultLimits();
  loose.max_dimension = 2048;
  Source big;
  ASSERT_EQ(Open("vp9_2048.webm", &big, &loose), WT_OK);
  ASSERT_EQ(wt_probe(big.p, &info), WT_OK);
  EXPECT_EQ(info.width, 2048u);
  EXPECT_EQ(info.frame_count, 2u);
}

TEST(ProbeErrorTest, ProbeAndAnimCreateAgree) {
  for (const Json& file : wt_test::Manifest()["files"].array) {
    const std::string& name = file["name"].string;
    SCOPED_TRACE(name);
    Source source;
    ASSERT_EQ(Open(name, &source), WT_OK);
    wt_info info;
    const wt_status probed = wt_probe(source.p, &info);
    Anim anim;
    EXPECT_EQ(wt_anim_create(source.p, 0, 0, &anim.p), probed);
    if (probed != WT_OK) continue;
    wt_info from_anim;
    ASSERT_EQ(wt_anim_get_info(anim.p, &from_anim, nullptr, nullptr), WT_OK);
    EXPECT_EQ(std::memcmp(&info, &from_anim, sizeof(info)), 0);
  }
}

TEST(ProbeErrorTest, TruncatedBeforeTheFirstFrame) {
  // The first 1 KiB of a real file holds the headers but no complete frame.
  const std::string path = (kAssets / "vp9_alpha_512.webm").string();
  FILE* f = std::fopen(path.c_str(), "rb");
  ASSERT_NE(f, nullptr);
  std::vector<uint8_t> head(1024);
  head.resize(std::fread(head.data(), 1, head.size(), f));
  std::fclose(f);
  Source source;
  ASSERT_EQ(
      wt_source_create_memory(head.data(), head.size(), nullptr, &source.p),
      WT_OK);
  wt_info info;
  EXPECT_EQ(wt_probe(source.p, &info), WT_ERR_TRUNCATED);
  EXPECT_TRUE(IsZero(info));
}

// --- Each limit, at and past its bound -------------------------------------

// vp9_alpha_512.webm: 512 x 512, 90 frames, about 3 s.
TEST(LimitTest, MaxBytes) {
  const uint64_t size = FileEntry("vp9_alpha_512.webm")["bytes"].Int();
  wt_limits limits = DefaultLimits();
  limits.max_bytes = size;
  Source source;
  EXPECT_EQ(Open("vp9_alpha_512.webm", &source, &limits), WT_OK);
  wt_info info;
  EXPECT_EQ(wt_probe(source.p, &info), WT_OK);

  limits.max_bytes = size - 1;
  Source over;
  EXPECT_EQ(Open("vp9_alpha_512.webm", &over, &limits), WT_ERR_LIMIT);
  EXPECT_EQ(over.p, nullptr);
}

TEST(LimitTest, MaxDimension) {
  wt_info info;
  wt_limits limits = DefaultLimits();
  limits.max_dimension = 512;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_OK);
  limits.max_dimension = 511;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_ERR_LIMIT);
  EXPECT_TRUE(IsZero(info));
  // Either axis counts: 301 x 199.
  limits.max_dimension = 301;
  EXPECT_EQ(Probe("vp9_odd_301x199.webm", &info, &limits), WT_OK);
  limits.max_dimension = 300;
  EXPECT_EQ(Probe("vp9_odd_301x199.webm", &info, &limits), WT_ERR_LIMIT);
}

TEST(LimitTest, MaxFrames) {
  wt_info info;
  wt_limits limits = DefaultLimits();
  limits.max_frames = 90;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_OK);
  limits.max_frames = 89;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_ERR_LIMIT);
  EXPECT_TRUE(IsZero(info));
}

TEST(LimitTest, MaxDurationMs) {
  wt_info info;
  ASSERT_EQ(Probe("vp9_alpha_512.webm", &info), WT_OK);
  const uint32_t duration = info.duration_ms;  // ~3000 ms.
  ASSERT_GT(duration, 100u);

  wt_limits limits = DefaultLimits();
  limits.max_duration_ms = duration + 1;  // Covers any rounding.
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_OK);
  limits.max_duration_ms = duration - 100;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_ERR_LIMIT);
  EXPECT_TRUE(IsZero(info));

  Anim anim;
  Source source;
  ASSERT_EQ(Open("vp9_alpha_512.webm", &source, &limits), WT_OK);
  EXPECT_EQ(wt_anim_create(source.p, 0, 0, &anim.p), WT_ERR_LIMIT);
}

// A truncated file is judged on the frames it still has.
TEST(LimitTest, DurationOfATruncatedFile) {
  wt_info info;
  ASSERT_EQ(Probe("truncated.webm", &info), WT_OK);
  wt_limits limits = DefaultLimits();
  limits.max_duration_ms = info.duration_ms + 1;
  EXPECT_EQ(Probe("truncated.webm", &info, &limits), WT_OK);
  limits.max_duration_ms = info.duration_ms / 2;
  EXPECT_EQ(Probe("truncated.webm", &info, &limits), WT_ERR_LIMIT)
      << "a limit error wins over truncation";
}

TEST(LimitTest, MaxTotalPixels) {
  // 512 x 512 x 90 frames.
  constexpr uint64_t kPixels = uint64_t{512} * 512 * 90;
  wt_info info;
  wt_limits limits = DefaultLimits();
  limits.max_total_pixels = kPixels;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_OK);
  limits.max_total_pixels = kPixels - 1;
  EXPECT_EQ(Probe("vp9_alpha_512.webm", &info, &limits), WT_ERR_LIMIT);
  EXPECT_TRUE(IsZero(info));

  Source source;
  Anim anim;
  ASSERT_EQ(Open("vp9_alpha_512.webm", &source, &limits), WT_OK);
  EXPECT_EQ(wt_anim_create(source.p, 64, 64, &anim.p), WT_ERR_LIMIT)
      << "the budget is at source size, whatever the output size";
}

// The default budget is what PLAN.md promises: 600 frames at 512 x 512.
TEST(LimitTest, DefaultBudgetCoversTheLongestDefaultSticker) {
  const wt_limits limits = DefaultLimits();
  EXPECT_GE(limits.max_total_pixels, uint64_t{512} * 512 * limits.max_frames);
}

TEST(LimitTest, ZeroFieldsAreInvalidArguments) {
  const uint8_t byte = 0;
  Source source;
  wt_limits limits = DefaultLimits();
  limits.max_total_pixels = 0;
  EXPECT_EQ(wt_source_create_memory(&byte, 1, &limits, &source.p),
            WT_ERR_INVALID_ARGUMENT);
  limits = DefaultLimits();
  limits.max_duration_ms = 0;
  EXPECT_EQ(wt_source_create_memory(&byte, 1, &limits, &source.p),
            WT_ERR_INVALID_ARGUMENT);
}

// The limits travel with the source: one set of bytes, two sources, two
// answers.
TEST(LimitTest, LimitsBelongToTheSource) {
  wt_limits tight = DefaultLimits();
  tight.max_frames = 10;
  Source a, b;
  ASSERT_EQ(Open("vp9_alpha_512.webm", &a, &tight), WT_OK);
  ASSERT_EQ(Open("vp9_alpha_512.webm", &b), WT_OK);
  wt_info info;
  EXPECT_EQ(wt_probe(a.p, &info), WT_ERR_LIMIT);
  EXPECT_EQ(wt_probe(b.p, &info), WT_OK);
}

}  // namespace
