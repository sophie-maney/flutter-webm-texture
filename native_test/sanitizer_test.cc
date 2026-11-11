// Checks that a WT_SANITIZE build really runs under ASan + UBSan, so a green
// CI run means something: the build must be instrumented, and both sanitizers
// must abort the process on a bug.
#include <cstdint>
#include <cstdlib>

#include "gtest/gtest.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WT_HAS_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define WT_HAS_ASAN 1
#endif
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WT_HAS_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define WT_HAS_TSAN 1
#endif

namespace {

#if defined(WT_TSAN)

// A WT_TSAN build (the scheduler's race check) must really be instrumented.
TEST(SanitizerTest, TsanIsCompiledIn) {
#if defined(WT_HAS_TSAN)
  SUCCEED();
#else
  FAIL() << "WT_TSAN is set but TSan is not compiled in";
#endif
}

#endif  // WT_TSAN

#if defined(WT_SANITIZE)

TEST(SanitizerTest, AsanIsCompiledIn) {
#if defined(WT_HAS_ASAN)
  SUCCEED();
#else
  FAIL() << "WT_SANITIZE is set but ASan is not compiled in";
#endif
}

// Results go to a volatile sink and inputs come through volatile, so the
// compiler can neither fold the bugs away nor drop them as dead code (GCC
// removes an unused overflowing add, checks included).
volatile int g_sink;

// UBSan is off here so the report comes from ASan: GCC's UBSan object-size
// check would otherwise catch the read first.
__attribute__((no_sanitize("undefined"))) void HeapOverflow(
    volatile int index) {
  volatile int* p = static_cast<int*>(std::calloc(4, sizeof(int)));
  g_sink = p[index];
  std::free(const_cast<int*>(p));
}

void SignedOverflow(volatile int x) { g_sink = x + 1; }

TEST(SanitizerDeathTest, AsanCatchesHeapOverflow) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(HeapOverflow(4), "heap-buffer-overflow");
}

TEST(SanitizerDeathTest, UbsanCatchesSignedOverflow) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(SignedOverflow(INT32_MAX), "signed integer overflow");
}

#else

TEST(SanitizerTest, DISABLED_NotASanitizerBuild) {}

#endif

}  // namespace
