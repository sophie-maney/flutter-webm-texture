// Reference frames in test_assets/ref (ADR 0001) and image comparisons for
// the decode tests: gzipped, premultiplied RGBA8888, stride = width * 4.
#ifndef WT_NATIVE_TEST_REF_FRAMES_H_
#define WT_NATIVE_TEST_REF_FRAMES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wt_test {

// Reads ref/<name>/<frame:03d>.rgba.gz. False when the file is missing,
// unreadable or not exactly width * height * 4 bytes once inflated.
bool LoadRefFrame(const std::string& name, int frame, uint32_t width,
                  uint32_t height, std::vector<uint8_t>* rgba);

// PSNR in dB over the channels of 4-byte pixels selected by `mask` (bit i
// for byte i of each pixel). Identical inputs give +infinity.
double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
            unsigned mask);

// Largest absolute difference over the masked channels.
int MaxError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
             unsigned mask);

// Box-filters 4-byte pixels down by an integer factor (width and height
// must be multiples of it), averaging with rounding.
std::vector<uint8_t> BoxDownscale(const std::vector<uint8_t>& rgba,
                                  uint32_t width, uint32_t height,
                                  uint32_t factor);

}  // namespace wt_test

#endif  // WT_NATIVE_TEST_REF_FRAMES_H_
