// Smoke test for the prebuilt dependency bundle.
//
// The build scripts compile and link this against every slice, so an archive
// with a missing object or an unresolved symbol fails the build. Run with the
// test_assets directory as its argument and it also exercises each library on
// the corpus, e.g. on a booted iOS Simulator:
//
//   xcrun simctl spawn booted ./smoke /path/to/test_assets
//
// This is a build check, not a correctness test: golden tests live in
// native_test/.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gif_lib.h"
#include "libyuv.h"
#include "lz4.h"
#include "lz4hc.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "webm/callback.h"
#include "webm/file_reader.h"
#include "webm/mkvparser/mkvparser.h"
#include "webm/mkvparser/mkvreader.h"
#include "webm/webm_parser.h"
#include "webp/decode.h"
#include "webp/demux.h"

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

// Collects the first frame of track 1 and its BlockAdditional (alpha), if any.
class FirstFrame : public webm::Callback {
 public:
  std::vector<std::uint8_t> frame;
  std::vector<std::uint8_t> alpha;
  int frames = 0;

  webm::Status OnBlockGroupEnd(const webm::ElementMetadata&,
                               const webm::BlockGroup& group) override {
    if (alpha.empty() && frames == 1 && group.additions.is_present()) {
      for (const auto& more : group.additions.value().block_mores) {
        if (more.value().id.value() == 1) alpha = more.value().data.value();
      }
    }
    return webm::Status(webm::Status::kOkCompleted);
  }

  webm::Status OnFrame(const webm::FrameMetadata& meta, webm::Reader* reader,
                       std::uint64_t* bytes_remaining) override {
    ++frames;
    if (!frame.empty()) {
      return webm::Callback::OnFrame(meta, reader, bytes_remaining);
    }
    frame.resize(static_cast<std::size_t>(*bytes_remaining));
    std::size_t off = 0;
    while (*bytes_remaining > 0) {
      std::uint64_t got = 0;
      webm::Status s = reader->Read(static_cast<std::size_t>(*bytes_remaining),
                                    frame.data() + off, &got);
      off += static_cast<std::size_t>(got);
      *bytes_remaining -= got;
      if (!s.completed_ok() && s.code != webm::Status::kOkPartial) return s;
    }
    return webm::Status(webm::Status::kOkCompleted);
  }
};

bool DecodeVp9(const std::vector<std::uint8_t>& data, unsigned* w,
               unsigned* h) {
  vpx_codec_ctx_t ctx;
  if (vpx_codec_dec_init(&ctx, vpx_codec_vp9_dx(), nullptr, 0)) return false;
  bool ok =
      vpx_codec_decode(&ctx, data.data(), static_cast<unsigned>(data.size()),
                       nullptr, 0) == VPX_CODEC_OK;
  vpx_codec_iter_t iter = nullptr;
  vpx_image_t* img = ok ? vpx_codec_get_frame(&ctx, &iter) : nullptr;
  if (img) {
    *w = img->d_w;
    *h = img->d_h;
    // Convert to premultiplied ARGB with an opaque alpha plane.
    std::vector<std::uint8_t> a(img->d_w * img->d_h, 255);
    std::vector<std::uint8_t> argb(img->d_w * img->d_h * 4);
    ok = libyuv::I420AlphaToARGB(img->planes[0], img->stride[0], img->planes[1],
                                 img->stride[1], img->planes[2], img->stride[2],
                                 a.data(), static_cast<int>(img->d_w),
                                 argb.data(), static_cast<int>(img->d_w * 4),
                                 static_cast<int>(img->d_w),
                                 static_cast<int>(img->d_h), 1) == 0 &&
         argb[3] == 255;
  }
  vpx_codec_destroy(&ctx);
  return ok && img != nullptr;
}

void RunCorpus(const std::string& dir) {
  // libwebm webm_parser + libvpx + libyuv.
  {
    std::string path = dir + "/vp9_alpha_512.webm";
    FILE* f = std::fopen(path.c_str(), "rb");
    Check(f != nullptr, "open vp9_alpha_512.webm");
    if (f) {
      webm::FileReader reader(f);
      FirstFrame cb;
      webm::WebmParser parser;
      webm::Status s = parser.Feed(&cb, &reader);
      Check(s.completed_ok() && cb.frames > 1, "webm_parser reads all frames");
      Check(!cb.alpha.empty(), "webm_parser surfaces BlockAdditional ID 1");
      unsigned w = 0, h = 0;
      Check(DecodeVp9(cb.frame, &w, &h) && w == 512 && h == 512,
            "libvpx decodes 512x512 colour frame, libyuv converts it");
      unsigned aw = 0, ah = 0;
      Check(!cb.alpha.empty() && DecodeVp9(cb.alpha, &aw, &ah) && aw == 512,
            "libvpx decodes alpha frame");
    }
  }
  // libvpx was built with --size-limit=1024x1024.
  {
    std::string path = dir + "/vp9_2048.webm";
    FILE* f = std::fopen(path.c_str(), "rb");
    Check(f != nullptr, "open vp9_2048.webm");
    if (f) {
      webm::FileReader reader(f);
      FirstFrame cb;
      webm::WebmParser parser;
      parser.Feed(&cb, &reader);
      unsigned w = 0, h = 0;
      Check(!cb.frame.empty() && !DecodeVp9(cb.frame, &w, &h),
            "libvpx rejects a 2048x2048 frame (--size-limit)");
    }
  }
  // libwebm mkvparser.
  {
    mkvparser::MkvReader reader;
    std::string path = dir + "/vp8_alpha_256.webm";
    long long pos = 0;
    mkvparser::EBMLHeader header;
    mkvparser::Segment* segment = nullptr;
    bool ok =
        reader.Open(path.c_str()) == 0 && header.Parse(&reader, pos) == 0 &&
        mkvparser::Segment::CreateInstance(&reader, pos, segment) == 0 &&
        segment->Load() >= 0 && segment->GetTracks()->GetTracksCount() >= 1;
    Check(ok, "mkvparser loads vp8_alpha_256.webm");
    delete segment;
  }
  // libwebp anim decoder.
  {
    std::string path = dir + "/anim.webp";
    std::vector<std::uint8_t> bytes;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      std::uint8_t buf[4096];
      std::size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        bytes.insert(bytes.end(), buf, buf + n);
      }
      std::fclose(f);
    }
    WebPData data = {bytes.data(), bytes.size()};
    WebPAnimDecoderOptions opts;
    WebPAnimDecoderOptionsInit(&opts);
    opts.color_mode = MODE_rgbA;
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &opts);
    WebPAnimInfo info;
    int frames = 0;
    if (dec && WebPAnimDecoderGetInfo(dec, &info)) {
      std::uint8_t* rgba;
      int ts;
      while (WebPAnimDecoderHasMoreFrames(dec) &&
             WebPAnimDecoderGetNext(dec, &rgba, &ts)) {
        ++frames;
      }
    }
    Check(dec && frames > 1 && frames == static_cast<int>(info.frame_count),
          "WebPAnimDecoder decodes every anim.webp frame");
    WebPAnimDecoderDelete(dec);
  }
  // giflib.
  {
    std::string path = dir + "/anim_disposal.gif";
    int err = 0;
    GifFileType* gif = DGifOpenFileName(path.c_str(), &err);
    Check(gif && DGifSlurp(gif) == GIF_OK && gif->ImageCount > 1,
          "giflib slurps anim_disposal.gif");
    if (gif) DGifCloseFile(gif, &err);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::printf(
      "libvpx %s, libwebp decoder %06x, libyuv %d, giflib %d.%d.%d, "
      "lz4 %s\n",
      vpx_codec_version_str(), WebPGetDecoderVersion(), LIBYUV_VERSION,
      GIFLIB_MAJOR, GIFLIB_MINOR, GIFLIB_RELEASE, LZ4_versionString());

  // lz4 + lz4hc round trip.
  {
    std::vector<char> src(64 * 1024);
    for (std::size_t i = 0; i < src.size(); ++i) {
      src[i] = static_cast<char>((i * 7) % 61);
    }
    std::vector<char> packed(LZ4_compressBound(static_cast<int>(src.size())));
    std::vector<char> out(src.size());
    int n =
        LZ4_compress_HC(src.data(), packed.data(), static_cast<int>(src.size()),
                        static_cast<int>(packed.size()), LZ4HC_CLEVEL_DEFAULT);
    int m = LZ4_decompress_safe(packed.data(), out.data(), n,
                                static_cast<int>(out.size()));
    Check(n > 0 && m == static_cast<int>(src.size()) && out == src,
          "lz4 round trip");
  }

  if (argc > 1) RunCorpus(argv[1]);
  std::printf("%s\n", failures ? "SMOKE FAILED" : "smoke passed");
  return failures ? 1 : 0;
}
