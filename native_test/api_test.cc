// Tests for the C ABI surface in src/api/wt_api.h (T1.1: argument checks and
// stubs). Later tasks replace the WT_ERR_UNIMPLEMENTED expectations with real
// behaviour.
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "gtest/gtest.h"
#include "wt_api.h"

extern "C" int wt_c_abi_check(void);

namespace {

// Every status code the header defines.
constexpr wt_status kAllStatuses[] = {
    WT_OK,
    WT_END,
    WT_ERR_INVALID_ARGUMENT,
    WT_ERR_UNIMPLEMENTED,
    WT_ERR_INVALID_DATA,
    WT_ERR_UNSUPPORTED,
    WT_ERR_TRUNCATED,
    WT_ERR_LIMIT,
    WT_ERR_NO_MEMORY,
    WT_ERR_IO,
    WT_ERR_DECODE,
    WT_ERR_INTERNAL,
};

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

TEST(ApiTest, VersionMatchesHeader) {
  EXPECT_EQ(wt_api_version(), WT_API_VERSION);
}

TEST(ApiTest, HeaderWorksFromC) { EXPECT_EQ(wt_c_abi_check(), 0); }

TEST(ApiTest, StatusSignConvention) {
  for (wt_status s : kAllStatuses) {
    const std::string name = wt_status_string(s);
    if (s < 0) {
      EXPECT_EQ(name.rfind("WT_ERR_", 0), 0u) << name;
    } else {
      EXPECT_EQ(name.rfind("WT_ERR_", 0), std::string::npos) << name;
    }
  }
}

TEST(ApiTest, StatusStringsAreDistinct) {
  std::set<std::string> names;
  for (wt_status s : kAllStatuses) {
    const char* name = wt_status_string(s);
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(names.insert(name).second) << "duplicate " << name;
    EXPECT_STRNE(name, "WT_ERR_?");
  }
}

TEST(ApiTest, UnknownStatusString) {
  EXPECT_STREQ(wt_status_string(-12345), "WT_ERR_?");
  EXPECT_STREQ(wt_status_string(99), "WT_ERR_?");
}

// The defaults PLAN.md's StickerLimits documents: 1024 px, 10 s, 2 MiB.
TEST(ApiTest, DefaultLimits) {
  wt_limits limits;
  std::memset(&limits, 0xAB, sizeof(limits));
  wt_limits_init_default(&limits);
  EXPECT_EQ(limits.max_bytes, uint64_t{2} << 20);
  EXPECT_EQ(limits.max_total_pixels, uint64_t{160} << 20);
  EXPECT_EQ(limits.max_dimension, 1024u);
  EXPECT_EQ(limits.max_frames, 600u);
  EXPECT_EQ(limits.max_duration_ms, 10000u);
  // The budget covers the longest sticker the other limits allow at 512 px.
  EXPECT_GE(limits.max_total_pixels, uint64_t{512} * 512 * limits.max_frames);
  wt_limits_init_default(nullptr);  // No crash.
}

TEST(ApiTest, DestroyNullIsNoOp) {
  wt_source_destroy(nullptr);
  wt_anim_destroy(nullptr);
}

TEST(SourceTest, MemoryArgumentChecks) {
  const uint8_t byte = 0;
  const wt_limits limits = DefaultLimits();
  wt_source* source = reinterpret_cast<wt_source*>(uintptr_t{1});

  EXPECT_EQ(wt_source_create_memory(&byte, 1, &limits, nullptr),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_source_create_memory(nullptr, 1, &limits, &source),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(source, nullptr) << "out handle is cleared on failure";

  wt_limits zero_field = limits;
  zero_field.max_frames = 0;
  source = reinterpret_cast<wt_source*>(uintptr_t{1});
  EXPECT_EQ(wt_source_create_memory(&byte, 1, &zero_field, &source),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(source, nullptr);
}

TEST(SourceTest, FileArgumentChecks) {
  wt_source* source = reinterpret_cast<wt_source*>(uintptr_t{1});
  EXPECT_EQ(wt_source_create_file("x.webm", nullptr, nullptr),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_source_create_file(nullptr, nullptr, &source),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(wt_source_create_file("", nullptr, &source),
            WT_ERR_INVALID_ARGUMENT);
}

TEST(ProbeTest, ArgumentChecks) {
  wt_info info;
  std::memset(&info, 0xAB, sizeof(info));
  EXPECT_EQ(wt_probe(nullptr, &info), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(info.width, 0u) << "info is zeroed on failure";
  EXPECT_FALSE(info.has_alpha);
  EXPECT_EQ(wt_probe(nullptr, nullptr), WT_ERR_INVALID_ARGUMENT);
}

TEST(AnimTest, CreateArgumentChecks) {
  wt_anim* anim = reinterpret_cast<wt_anim*>(uintptr_t{1});
  EXPECT_EQ(wt_anim_create(nullptr, 0, 0, &anim), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(anim, nullptr) << "out handle is cleared on failure";
  EXPECT_EQ(wt_anim_create(nullptr, 64, 64, nullptr), WT_ERR_INVALID_ARGUMENT);
}

TEST(AnimTest, NullHandleChecks) {
  wt_info info;
  uint32_t w = 7, h = 7;
  EXPECT_EQ(wt_anim_get_info(nullptr, &info, &w, &h), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(w, 0u);
  EXPECT_EQ(h, 0u);
  EXPECT_EQ(wt_anim_get_info(nullptr, nullptr, nullptr, nullptr),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_anim_seek(nullptr, 0), WT_ERR_INVALID_ARGUMENT);

  uint8_t pixels[16] = {};
  wt_frame_info frame;
  EXPECT_EQ(wt_anim_next_frame(nullptr, pixels, sizeof(pixels), 8,
                               WT_PIXEL_FORMAT_RGBA8888, &frame),
            WT_ERR_INVALID_ARGUMENT);
}

}  // namespace
