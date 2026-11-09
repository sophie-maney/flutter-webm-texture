// Spike A (T0.7): a minimal driver for comparing demuxer code size. The
// "none" build is the baseline; subtract it from the other two.
#include <cstdio>
#include <vector>

#include "frame_index.h"

namespace spike {
bool g_libwebm_unguarded = false;
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  std::FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 1;
  std::vector<unsigned char> d;
  int c;
  while ((c = std::fgetc(f)) != EOF) d.push_back(static_cast<unsigned char>(c));
  std::fclose(f);
  spike::Limits lim;
#if defined(PROBE_LIBWEBM)
  spike::FrameIndex idx = spike::IndexWithLibwebm(d.data(), d.size(), lim);
#elif defined(PROBE_MINI)
  spike::FrameIndex idx = spike::IndexWithMiniEbml(d.data(), d.size(), lim);
#else
  spike::FrameIndex idx;
#endif
  std::printf("%zu frames\n", idx.frames.size());
  return 0;
}
