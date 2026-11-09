// Helpers shared by the wt_anim tests (anim_test.cc, golden_test.cc): RAII
// owners for the C handles and a decode-next-frame wrapper that takes the
// buffer size from the animation.
#ifndef WT_NATIVE_TEST_ANIM_UTIL_H_
#define WT_NATIVE_TEST_ANIM_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "wt_api.h"

namespace wt_test {

using Bytes = std::vector<uint8_t>;

struct Source {
  wt_source* p = nullptr;
  ~Source() { wt_source_destroy(p); }
};

struct Anim {
  wt_anim* p = nullptr;
  ~Anim() { wt_anim_destroy(p); }
};

struct Size {
  uint32_t w = 0, h = 0;
};

inline wt_status OpenSource(const std::string& name, Source* source,
                            const wt_limits* limits = nullptr) {
  const std::filesystem::path path =
      std::filesystem::path(WT_TEST_ASSETS_DIR) / name;
  return wt_source_create_file(path.string().c_str(), limits, &source->p);
}

// Opens name at an output size of w x h (0 x 0 for the source size).
inline wt_status OpenAnim(const std::string& name, uint32_t w, uint32_t h,
                          Anim* anim) {
  Source source;
  const wt_status s = OpenSource(name, &source);
  if (s != WT_OK) return s;
  return wt_anim_create(source.p, w, h, &anim->p);  // Source dies here.
}

inline Size OutputSize(const Anim& anim) {
  Size size;
  EXPECT_EQ(wt_anim_get_info(anim.p, nullptr, &size.w, &size.h), WT_OK);
  return size;
}

// Decodes the next frame into a tight buffer.
inline wt_status Next(const Anim& anim, Bytes* pixels,
                      wt_pixel_format format = WT_PIXEL_FORMAT_RGBA8888,
                      wt_frame_info* info = nullptr) {
  const Size size = OutputSize(anim);
  pixels->assign(size_t{size.w} * size.h * 4, 0xCD);
  return wt_anim_next_frame(anim.p, pixels->data(), pixels->size(), size.w * 4,
                            format, info);
}

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_ANIM_UTIL_H_
