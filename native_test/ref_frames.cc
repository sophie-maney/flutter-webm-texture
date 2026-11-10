#include "ref_frames.h"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace wt_test {

bool LoadRefFrame(const std::string& name, int frame, uint32_t width,
                  uint32_t height, std::vector<uint8_t>* rgba) {
  char file[16];
  std::snprintf(file, sizeof(file), "%03d.rgba.gz", frame);
  const std::string path =
      std::string(WT_TEST_ASSETS_DIR) + "/ref/" + name + "/" + file;
  gzFile gz = gzopen(path.c_str(), "rb");
  if (gz == nullptr) return false;
  const size_t want = size_t{width} * height * 4;
  rgba->assign(want + 1, 0);
  // Read one byte more than expected, to catch a reference that is too big.
  size_t got = 0;
  while (got < rgba->size()) {
    const int n = gzread(gz, rgba->data() + got,
                         static_cast<unsigned>(rgba->size() - got));
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  gzclose(gz);
  rgba->resize(want);
  return got == want;
}

double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
            unsigned mask) {
  if (a.size() != b.size() || a.empty()) return 0;
  double sse = 0;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if ((mask & (1u << (i % 4))) == 0) continue;
    const double d = static_cast<double>(a[i]) - b[i];
    sse += d * d;
    ++n;
  }
  if (sse == 0) return std::numeric_limits<double>::infinity();
  return 10 * std::log10(255.0 * 255.0 * static_cast<double>(n) / sse);
}

int MaxError(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
             unsigned mask) {
  if (a.size() != b.size()) return 256;
  int max = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if ((mask & (1u << (i % 4))) == 0) continue;
    const int d = std::abs(int{a[i]} - int{b[i]});
    if (d > max) max = d;
  }
  return max;
}

std::vector<uint8_t> BoxDownscale(const std::vector<uint8_t>& rgba,
                                  uint32_t width, uint32_t height,
                                  uint32_t factor) {
  const uint32_t w = width / factor, h = height / factor;
  std::vector<uint8_t> out(size_t{w} * h * 4);
  const uint32_t area = factor * factor;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (uint32_t c = 0; c < 4; ++c) {
        uint32_t sum = 0;
        for (uint32_t dy = 0; dy < factor; ++dy) {
          for (uint32_t dx = 0; dx < factor; ++dx) {
            sum +=
                rgba[((size_t{y} * factor + dy) * width + x * factor + dx) * 4 +
                     c];
          }
        }
        out[(size_t{y} * w + x) * 4 + c] =
            static_cast<uint8_t>((sum + area / 2) / area);
      }
    }
  }
  return out;
}

std::vector<uint8_t> AreaDownscale(const std::vector<uint8_t>& rgba,
                                   uint32_t width, uint32_t height,
                                   uint32_t dst_w, uint32_t dst_h) {
  std::vector<uint8_t> out(size_t{dst_w} * dst_h * 4);
  const double sx = static_cast<double>(width) / dst_w;
  const double sy = static_cast<double>(height) / dst_h;
  for (uint32_t y = 0; y < dst_h; ++y) {
    const double y0 = y * sy, y1 = y0 + sy;
    for (uint32_t x = 0; x < dst_w; ++x) {
      const double x0 = x * sx, x1 = x0 + sx;
      double sum[4] = {0, 0, 0, 0};
      for (uint32_t j = static_cast<uint32_t>(y0);
           j < height && static_cast<double>(j) < y1; ++j) {
        const double wy = std::fmin(j + 1.0, y1) - std::fmax(j * 1.0, y0);
        for (uint32_t i = static_cast<uint32_t>(x0);
             i < width && static_cast<double>(i) < x1; ++i) {
          const double w =
              wy * (std::fmin(i + 1.0, x1) - std::fmax(i * 1.0, x0));
          for (int c = 0; c < 4; ++c) {
            sum[c] += w * rgba[(size_t{j} * width + i) * 4 + c];
          }
        }
      }
      for (int c = 0; c < 4; ++c) {
        out[(size_t{y} * dst_w + x) * 4 + c] =
            static_cast<uint8_t>(std::lround(sum[c] / (sx * sy)));
      }
    }
  }
  return out;
}

}  // namespace wt_test
