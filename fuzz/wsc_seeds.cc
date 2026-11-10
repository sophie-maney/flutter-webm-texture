// Writes the fuzz_wsc seed corpus: small, valid .wsc files made by playing
// corpus clips through wt_anim_create_cached. Built with the fuzzers.
//
//   build/fuzz/wsc_seeds test_assets fuzz/corpus/wsc
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "wt_api.h"

namespace {

struct Seed {
  const char* clip;
  uint32_t w, h;
  const char* out;
};

// Sizes and frame counts within fuzz_wsc's limits (256 px, 64 frames).
constexpr Seed kSeeds[] = {
    {"vp8_alpha_256.webm", 16, 16, "vp8_alpha_16x16.wsc"},
    {"vp9_alpha_vfr.webm", 24, 24, "vp9_alpha_vfr_24x24.wsc"},
    {"vp9_odd_301x199.webm", 15, 10, "vp9_opaque_15x10.wsc"},
    {"vp9_single_frame.webm", 32, 32, "vp9_single_32x32.wsc"},
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <test_assets> <out dir>\n", argv[0]);
    return 2;
  }
  for (const Seed& seed : kSeeds) {
    const std::string in = std::string(argv[1]) + "/" + seed.clip;
    const std::string out = std::string(argv[2]) + "/" + seed.out;
    std::remove(out.c_str());
    wt_source* source = nullptr;
    wt_anim* anim = nullptr;
    wt_status s = wt_source_create_file(in.c_str(), nullptr, &source);
    if (s == WT_OK) {
      s = wt_anim_create_cached(source, seed.w, seed.h, out.c_str(), &anim);
    }
    std::vector<uint8_t> px(size_t{seed.w} * seed.h * 4);
    while (s == WT_OK) {
      s = wt_anim_next_frame(anim, px.data(), px.size(), seed.w * 4,
                             WT_PIXEL_FORMAT_RGBA8888, nullptr);
    }
    wt_anim_destroy(anim);
    wt_source_destroy(source);
    if (s != WT_END) {
      std::fprintf(stderr, "%s: %s\n", seed.clip, wt_status_string(s));
      return 1;
    }
    std::printf("wrote %s\n", out.c_str());
  }
  return 0;
}
