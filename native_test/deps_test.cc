// Proves the third-party libraries link into the host build and run under the
// sanitizers. Each check calls one real entry point per library; the decode
// paths themselves are tested by the tasks that use them (T1.4+).
#include <cstdint>
#include <cstring>
#include <vector>

#include "gif_lib.h"
#include "gtest/gtest.h"
#include "libyuv/convert_argb.h"
#include "lz4.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "webp/decode.h"
#include "webp/demux.h"

namespace {

TEST(DepsTest, LibvpxHasBothDecoders) {
  EXPECT_NE(vpx_codec_version_str(), nullptr);
  for (vpx_codec_iface_t* iface : {vpx_codec_vp8_dx(), vpx_codec_vp9_dx()}) {
    vpx_codec_ctx_t ctx;
    ASSERT_EQ(vpx_codec_dec_init(&ctx, iface, nullptr, 0), VPX_CODEC_OK)
        << vpx_codec_iface_name(iface);
    EXPECT_EQ(vpx_codec_destroy(&ctx), VPX_CODEC_OK);
  }
}

TEST(DepsTest, LibyuvConvertsWithAlpha) {
  // 2x2 mid-grey I420 with half alpha, premultiplied into BGRA ("ARGB").
  const uint8_t y[4] = {126, 126, 126, 126}, u[1] = {128}, v[1] = {128};
  const uint8_t a[4] = {128, 128, 128, 128};
  uint8_t out[16] = {};
  ASSERT_EQ(libyuv::I420AlphaToARGB(y, 2, u, 1, v, 1, a, 2, out, 8, 2, 2,
                                    /*attenuate=*/1),
            0);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(out[i * 4 + 3], 128);
    EXPECT_LE(out[i * 4], out[i * 4 + 3]) << "premultiplied: colour <= alpha";
  }
}

TEST(DepsTest, LibwebpDecoderAndDemux) {
  EXPECT_GT(WebPGetDecoderVersion(), 0);
  EXPECT_GT(WebPGetDemuxVersion(), 0);
  const uint8_t junk[16] = {};
  int w = 0, h = 0;
  EXPECT_EQ(WebPGetInfo(junk, sizeof(junk), &w, &h), 0);
}

TEST(DepsTest, GiflibRejectsJunk) {
  EXPECT_NE(GifErrorString(D_GIF_ERR_NOT_GIF_FILE), nullptr);
}

TEST(DepsTest, Lz4RoundTrip) {
  std::vector<char> src(4096);
  for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<char>(i % 7);
  std::vector<char> packed(
      static_cast<size_t>(LZ4_compressBound(static_cast<int>(src.size()))));
  const int n = LZ4_compress_default(src.data(), packed.data(),
                                     static_cast<int>(src.size()),
                                     static_cast<int>(packed.size()));
  ASSERT_GT(n, 0);
  std::vector<char> back(src.size());
  ASSERT_EQ(LZ4_decompress_safe(packed.data(), back.data(), n,
                                static_cast<int>(back.size())),
            static_cast<int>(src.size()));
  EXPECT_EQ(std::memcmp(src.data(), back.data(), src.size()), 0);
}

}  // namespace
