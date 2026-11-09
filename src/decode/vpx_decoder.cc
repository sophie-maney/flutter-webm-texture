#include "decode/vpx_decoder.h"

#include <climits>
#include <cstring>
#include <new>

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

namespace wt {

namespace {

vpx_codec_iface_t* Iface(wt_codec codec) {
  return codec == WT_CODEC_VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
}

YuvColor ColorOf(const vpx_image_t& img) {
  YuvColor c;
  switch (img.cs) {
    case VPX_CS_BT_709:
    case VPX_CS_SMPTE_240:  // Close to BT.709's coefficients.
      c.matrix = YuvMatrix::kBt709;
      break;
    case VPX_CS_BT_2020:
      c.matrix = YuvMatrix::kBt2020;
      break;
    default:  // Unknown (always, for VP8), BT.601, SMPTE 170, reserved.
      c.matrix = YuvMatrix::kBt601;
      break;
  }
  c.full_range = img.range == VPX_CR_FULL_RANGE;
  return c;
}

// An 8-bit I420 image of exactly width x height, with sane planes.
wt_status CheckImage(const vpx_image_t& img, uint32_t width, uint32_t height) {
  if (img.fmt != VPX_IMG_FMT_I420 || img.bit_depth != 8) {
    return WT_ERR_UNSUPPORTED;
  }
  if (img.d_w != width || img.d_h != height) return WT_ERR_INVALID_DATA;
  for (int p = 0; p < 3; ++p) {
    const int min_stride = static_cast<int>(p == 0 ? width : ChromaSize(width));
    if (img.planes[p] == nullptr || img.stride[p] < min_stride) {
      return WT_ERR_INTERNAL;
    }
  }
  return WT_OK;
}

}  // namespace

// One libvpx decoder context, with the keyframe gating every stream needs.
class VpxDecoder::Context {
 public:
  Context() { std::memset(&ctx_, 0, sizeof(ctx_)); }
  ~Context() {
    if (ready_) vpx_codec_destroy(&ctx_);
  }
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;

  wt_status Init(wt_codec codec, uint32_t width, uint32_t height) {
    codec_ = codec;
    vpx_codec_dec_cfg_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    // One thread per context: the scheduler parallelises across
    // animations, not inside one.
    cfg.threads = 1;
    cfg.w = width;
    cfg.h = height;
    const vpx_codec_err_t err =
        vpx_codec_dec_init(&ctx_, Iface(codec), &cfg, 0);
    if (err != VPX_CODEC_OK) {
      return err == VPX_CODEC_MEM_ERROR ? WT_ERR_NO_MEMORY : WT_ERR_INTERNAL;
    }
    ready_ = true;
    return WT_OK;
  }

  enum class Result { kDecoded, kSkipped, kFailed, kNoMemory };

  // Decodes one packet unless the context is waiting for a keyframe and this
  // isn't one. On kDecoded, *img is the last shown image or null when the
  // packet showed none (superframes may also carry hidden frames).
  Result Decode(ByteView data, const vpx_image_t** img) {
    *img = nullptr;
    if (data.size == 0 || data.size > UINT_MAX) {
      need_keyframe_ = true;
      return Result::kFailed;
    }
    const unsigned int size = static_cast<unsigned int>(data.size);
    if (need_keyframe_) {
      vpx_codec_stream_info_t si;
      std::memset(&si, 0, sizeof(si));
      si.sz = sizeof(si);
      if (vpx_codec_peek_stream_info(Iface(codec_), data.data, size, &si) !=
              VPX_CODEC_OK ||
          !si.is_kf) {
        return Result::kSkipped;
      }
    }
    const vpx_codec_err_t err =
        vpx_codec_decode(&ctx_, data.data, size, nullptr, 0);
    if (err != VPX_CODEC_OK) {
      need_keyframe_ = true;
      return err == VPX_CODEC_MEM_ERROR ? Result::kNoMemory : Result::kFailed;
    }
    need_keyframe_ = false;
    vpx_codec_iter_t iter = nullptr;
    while (const vpx_image_t* frame = vpx_codec_get_frame(&ctx_, &iter)) {
      *img = frame;
    }
    return Result::kDecoded;
  }

  // The next packet must be a keyframe, e.g. after its stream had a gap.
  void RequireKeyframe() { need_keyframe_ = true; }

 private:
  vpx_codec_ctx_t ctx_;
  wt_codec codec_ = WT_CODEC_UNKNOWN;
  bool ready_ = false;
  bool need_keyframe_ = true;
};

VpxDecoder::VpxDecoder() = default;
VpxDecoder::~VpxDecoder() = default;

wt_status VpxDecoder::Init(wt_codec codec, uint32_t width, uint32_t height,
                           bool track_alpha) {
  if ((codec != WT_CODEC_VP8 && codec != WT_CODEC_VP9) || width == 0 ||
      height == 0 || width > YuvaImage::kMaxDimension ||
      height > YuvaImage::kMaxDimension || color_ != nullptr) {
    return WT_ERR_INVALID_ARGUMENT;
  }
  std::unique_ptr<Context> color(new (std::nothrow) Context);
  if (color == nullptr) return WT_ERR_NO_MEMORY;
  const wt_status s = color->Init(codec, width, height);
  if (s != WT_OK) return s;
  codec_ = codec;
  width_ = width;
  height_ = height;
  track_alpha_ = track_alpha;
  color_ = std::move(color);
  return WT_OK;
}

wt_status VpxDecoder::Decode(ByteView color, ByteView alpha, Output* out) {
  *out = Output{};
  if (color_ == nullptr) return WT_ERR_INTERNAL;

  const vpx_image_t* img = nullptr;
  switch (color_->Decode(color, &img)) {
    case Context::Result::kDecoded:
      break;
    case Context::Result::kNoMemory:
      return WT_ERR_NO_MEMORY;
    case Context::Result::kSkipped:
    case Context::Result::kFailed:
      // The alpha frame belongs to a colour frame we can't show; its stream
      // must restart at a keyframe too, or it drifts out of step.
      if (alpha_ != nullptr) alpha_->RequireKeyframe();
      return WT_ERR_DECODE;
  }

  YuvaView view;
  if (img != nullptr) {
    const wt_status s = CheckImage(*img, width_, height_);
    if (s != WT_OK) {
      if (alpha_ != nullptr) alpha_->RequireKeyframe();  // It misses a packet.
      return s;
    }
    view.y = img->planes[VPX_PLANE_Y];
    view.u = img->planes[VPX_PLANE_U];
    view.v = img->planes[VPX_PLANE_V];
    view.y_stride = img->stride[VPX_PLANE_Y];
    view.u_stride = img->stride[VPX_PLANE_U];
    view.v_stride = img->stride[VPX_PLANE_V];
    view.width = width_;
    view.height = height_;
    view.color = ColorOf(*img);
  }
  // Alpha is decoded even for a packet that shows no colour, so the alpha
  // context sees every packet its stream has.
  const uint8_t* a = nullptr;
  int a_stride = 0;
  const wt_status s = DecodeAlpha(alpha, &a, &a_stride);
  if (s != WT_OK) return s;
  if (img == nullptr) return WT_OK;  // Nothing shown: the caller repeats.
  out->shown = true;
  out->alpha_fresh = a != nullptr;
  out->view = view;
  out->view.a = a;
  out->view.a_stride = a_stride;
  return WT_OK;
}

wt_status VpxDecoder::DecodeAlpha(ByteView alpha, const uint8_t** plane,
                                  int* stride) {
  if (!track_alpha_) return WT_OK;  // Pitfall 3: ignore stray additions.
  if (alpha.size == 0) {
    // A gap: the next alpha packet is an inter frame whose reference we
    // never decoded.
    if (alpha_ != nullptr) alpha_->RequireKeyframe();
    return WT_OK;
  }
  if (alpha_ == nullptr) {
    std::unique_ptr<Context> ctx(new (std::nothrow) Context);
    if (ctx == nullptr) return WT_ERR_NO_MEMORY;
    if (ctx->Init(codec_, width_, height_) != WT_OK) {
      return WT_OK;  // No alpha decoder, so no alpha. Not worth a failure.
    }
    alpha_ = std::move(ctx);
  }
  const vpx_image_t* img = nullptr;
  const Context::Result r = alpha_->Decode(alpha, &img);
  if (r == Context::Result::kNoMemory) return WT_ERR_NO_MEMORY;
  if (r != Context::Result::kDecoded || img == nullptr) return WT_OK;
  // Pitfall 6: the alpha image must match the colour image. Only its Y
  // plane is used, raw (pitfall 7: no range expansion).
  if (CheckImage(*img, width_, height_) != WT_OK) {
    alpha_->RequireKeyframe();
    return WT_OK;
  }
  *plane = img->planes[VPX_PLANE_Y];
  *stride = img->stride[VPX_PLANE_Y];
  return WT_OK;
}

}  // namespace wt
