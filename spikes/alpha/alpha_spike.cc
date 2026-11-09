// Spike A (T0.7): throwaway host CLI for VP8/VP9 alpha demux + decode.
//
//   alpha_spike verify      <test_assets> <expect.txt>
//   alpha_spike experiments <test_assets>
//   alpha_spike stress      <test_assets> <iterations> [seed]
//   alpha_spike compare     <file.webm> <raw straight RGBA from ffmpeg>
//
// `verify` indexes every corpus WebM with both prototype demuxers (libwebm
// webm_parser and the minimal EBML reader), checks the two indexes agree and
// match manifest.json, decodes colour + alpha with two libvpx contexts, merges
// to premultiplied RGBA with libyuv and compares against test_assets/ref.
// `experiments` probes the pitfalls listed in docs/spikes/A-alpha.md.
// `stress` mutates the corpus and runs both demuxers plus a short decode, for
// a robustness comparison under ASan/UBSan. `compare` checks every frame of
// one file against an ffmpeg + libvpx decode (see allframes.sh).
//
// expect.txt is manifest.json flattened by run.sh (jq), one line per file:
//   name|kind|expect|codec|w|h|alpha|has_audio|frames|pts_ms,..|kf,..|ref,..
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "frame_index.h"
#include "libyuv.h"
#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "webm/dom_types.h"

namespace spike {

bool g_libwebm_unguarded = false;

const char* ToString(IndexStatus s) {
  switch (s) {
    case IndexStatus::kOk:
      return "ok";
    case IndexStatus::kTruncated:
      return "truncated";
    case IndexStatus::kInvalid:
      return "invalid";
    case IndexStatus::kUnsupported:
      return "unsupported";
    case IndexStatus::kLimit:
      return "limit";
  }
  return "?";
}

namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------------------
// Inputs

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

bool ReadGz(const std::string& path, std::size_t bytes,
            std::vector<std::uint8_t>* out) {
  gzFile f = gzopen(path.c_str(), "rb");
  if (!f) return false;
  out->assign(bytes + 1, 0);
  const int got = gzread(f, out->data(), static_cast<unsigned>(bytes + 1));
  gzclose(f);
  if (got != static_cast<int>(bytes)) return false;
  out->resize(bytes);
  return true;
}

struct Expect {
  std::string name, kind, expect, codec;
  int w = 0, h = 0;
  bool alpha = false, has_audio = false;
  int frames = 0;
  std::vector<int> pts_ms, keyframes, ref_frames;
};

std::vector<int> Ints(const std::string& s) {
  std::vector<int> v;
  std::stringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty()) v.push_back(std::atoi(tok.c_str()));
  }
  return v;
}

std::vector<Expect> LoadExpect(const std::string& path) {
  std::vector<Expect> out;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, '|')) c.push_back(tok);
    while (c.size() < 12) c.push_back("");
    Expect e;
    e.name = c[0];
    e.kind = c[1];
    e.expect = c[2];
    e.codec = c[3];
    e.w = std::atoi(c[4].c_str());
    e.h = std::atoi(c[5].c_str());
    e.alpha = c[6] == "true";
    e.has_audio = c[7] == "true";
    e.frames = std::atoi(c[8].c_str());
    e.pts_ms = Ints(c[9]);
    e.keyframes = Ints(c[10]);
    e.ref_frames = Ints(c[11]);
    out.push_back(e);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Decoding

class VpxDecoder {
 public:
  ~VpxDecoder() {
    if (ready_) vpx_codec_destroy(&ctx_);
  }
  bool Init(Codec codec) {
    // One thread per context: the production scheduler parallelises across
    // animations, not inside one.
    vpx_codec_dec_cfg_t cfg = {1, 0, 0};
    vpx_codec_iface_t* iface =
        codec == Codec::kVp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
    ready_ = vpx_codec_dec_init(&ctx_, iface, &cfg, 0) == VPX_CODEC_OK;
    return ready_;
  }
  // Decodes one packet. Returns the last shown image (VP9 superframes carry
  // hidden frames that produce none), or null with *err set.
  const vpx_image_t* Decode(const std::uint8_t* d, std::size_t n, int* shown,
                            std::string* err) {
    *shown = 0;
    if (vpx_codec_decode(&ctx_, d, static_cast<unsigned>(n), nullptr, 0) !=
        VPX_CODEC_OK) {
      const char* detail = vpx_codec_error_detail(&ctx_);
      *err = std::string(vpx_codec_error(&ctx_)) +
             (detail ? std::string(" (") + detail + ")" : "");
      return nullptr;
    }
    vpx_codec_iter_t it = nullptr;
    const vpx_image_t* last = nullptr;
    while (const vpx_image_t* img = vpx_codec_get_frame(&ctx_, &it)) {
      last = img;
      ++*shown;
    }
    return last;
  }

 private:
  vpx_codec_ctx_t ctx_{};
  bool ready_ = false;
};

bool PeekKeyframe(Codec codec, const std::uint8_t* d, std::size_t n) {
  vpx_codec_stream_info_t si;
  si.sz = sizeof(si);
  vpx_codec_iface_t* iface =
      codec == Codec::kVp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
  if (vpx_codec_peek_stream_info(iface, d, static_cast<unsigned>(n), &si) !=
      VPX_CODEC_OK) {
    return false;
  }
  return si.is_kf != 0;
}

enum class Merge { kAttenuate, kExactPremultiply, kBilinearAttenuate };

// YUVA420 → premultiplied RGBA8888 (libyuv "ABGR"). libyuv has no
// I420AlphaToABGRMatrixFilter, so ABGR is produced the way libyuv does it
// internally: swap U and V and use the mirrored (Yvu) matrix.
bool ToRgba(const vpx_image_t* c, const vpx_image_t* a, Merge mode,
            std::uint8_t* dst, int stride, std::string* err) {
  if (c->fmt != VPX_IMG_FMT_I420) {
    *err = "colour image is not I420 (VP9 profile 1-3?)";
    return false;
  }
  if (a &&
      (a->fmt != VPX_IMG_FMT_I420 || a->d_w != c->d_w || a->d_h != c->d_h)) {
    *err = "alpha image format/size differs from colour";
    return false;
  }
  const int w = static_cast<int>(c->d_w), h = static_cast<int>(c->d_h);
  const std::uint8_t* y = c->planes[VPX_PLANE_Y];
  const std::uint8_t* u = c->planes[VPX_PLANE_U];
  const std::uint8_t* v = c->planes[VPX_PLANE_V];
  const int sy = c->stride[VPX_PLANE_Y], su = c->stride[VPX_PLANE_U],
            sv = c->stride[VPX_PLANE_V];
  int r = 0;
  if (!a) {
    r = libyuv::I420ToARGBMatrix(y, sy, v, sv, u, su, dst, stride,
                                 &libyuv::kYvuI601Constants, w, h);
  } else {
    const std::uint8_t* ap = a->planes[VPX_PLANE_Y];
    const int sa = a->stride[VPX_PLANE_Y];
    switch (mode) {
      case Merge::kAttenuate:
        r = libyuv::I420AlphaToARGBMatrix(y, sy, v, sv, u, su, ap, sa, dst,
                                          stride, &libyuv::kYvuI601Constants, w,
                                          h, 1);
        break;
      case Merge::kBilinearAttenuate:
        r = libyuv::I420AlphaToARGBMatrixFilter(
            y, sy, v, sv, u, su, ap, sa, dst, stride,
            &libyuv::kYvuI601Constants, w, h, 1, libyuv::kFilterBilinear);
        break;
      case Merge::kExactPremultiply:
        r = libyuv::I420AlphaToARGBMatrix(y, sy, v, sv, u, su, ap, sa, dst,
                                          stride, &libyuv::kYvuI601Constants, w,
                                          h, 0);
        for (int j = 0; r == 0 && j < h; ++j) {
          std::uint8_t* p = dst + static_cast<std::size_t>(j) * stride;
          for (int i = 0; i < w; ++i, p += 4) {
            for (int k = 0; k < 3; ++k) p[k] = (p[k] * p[3] + 127) / 255;
          }
        }
        break;
    }
  }
  if (r != 0) *err = "libyuv conversion failed";
  return r == 0;
}

struct FrameTiming {
  double color_ms = 0, alpha_ms = 0, convert_ms = 0;
};

// Decodes a clip frame by frame from its index.
class ClipDecoder {
 public:
  ClipDecoder(const std::uint8_t* file, const FrameIndex& idx)
      : file_(file), idx_(idx) {}

  bool Init() { return color_.Init(idx_.video.codec); }

  // Decodes frame i (frames must be fed in decode order). With `out`, merges
  // into it. Returns false with *err on failure.
  bool Frame(std::size_t i, std::vector<std::uint8_t>* out, Merge mode,
             FrameTiming* t, std::string* err, bool drop_alpha = false) {
    const FrameEntry& f = idx_.frames[i];
    int shown = 0;
    auto t0 = Clock::now();
    const vpx_image_t* c =
        color_.Decode(file_ + f.data.offset, f.data.size, &shown, err);
    if (t) t->color_ms += MsSince(t0);
    if (!c) {
      if (err->empty()) *err = "no shown colour frame";
      return false;
    }
    if (c->d_w != idx_.video.width || c->d_h != idx_.video.height) {
      *err = "decoded size differs from track size";
      return false;
    }
    const vpx_image_t* a = nullptr;
    if (idx_.video.alpha_mode == 1 && f.alpha.present() && !drop_alpha) {
      if (!alpha_) {
        alpha_.reset(new VpxDecoder);
        if (!alpha_->Init(idx_.video.codec)) {
          *err = "alpha decoder init failed";
          return false;
        }
      }
      t0 = Clock::now();
      a = alpha_->Decode(file_ + f.alpha.offset, f.alpha.size, &shown, err);
      if (t) t->alpha_ms += MsSince(t0);
      if (!a) {
        *err = "alpha: " + (err->empty() ? std::string("no frame") : *err);
        return false;
      }
    }
    if (!out) return true;
    out->resize(static_cast<std::size_t>(c->d_w) * c->d_h * 4);
    t0 = Clock::now();
    const bool ok =
        ToRgba(c, a, mode, out->data(), static_cast<int>(c->d_w) * 4, err);
    if (t) t->convert_ms += MsSince(t0);
    return ok;
  }

 private:
  const std::uint8_t* file_;
  const FrameIndex& idx_;
  VpxDecoder color_;
  std::unique_ptr<VpxDecoder> alpha_;
};

// ---------------------------------------------------------------------------
// Comparison

struct Cmp {
  int max_a = 0, max_rgb = 0;
  std::uint64_t a_mismatch = 0;
  double mse_a = 0, mse_rgb = 0;
  bool premultiplied = true;  // r,g,b <= a everywhere.
};

double Psnr(double mse) {
  return mse == 0 ? INFINITY : 10.0 * std::log10(255.0 * 255.0 / mse);
}

Cmp Compare(const std::vector<std::uint8_t>& got,
            const std::vector<std::uint8_t>& ref) {
  Cmp c;
  const std::size_t px = ref.size() / 4;
  double sa = 0, srgb = 0;
  for (std::size_t i = 0; i < px; ++i) {
    const std::uint8_t* g = &got[i * 4];
    const std::uint8_t* r = &ref[i * 4];
    const int da = std::abs(g[3] - r[3]);
    c.max_a = std::max(c.max_a, da);
    c.a_mismatch += da != 0;
    sa += da * da;
    for (int k = 0; k < 3; ++k) {
      const int d = std::abs(g[k] - r[k]);
      c.max_rgb = std::max(c.max_rgb, d);
      srgb += d * d;
      if (g[k] > g[3]) c.premultiplied = false;
    }
  }
  c.mse_a = sa / px;
  c.mse_rgb = srgb / (px * 3);
  return c;
}

std::string Db(double psnr) {
  if (std::isinf(psnr)) return "exact";
  char b[32];
  std::snprintf(b, sizeof(b), "%.2f", psnr);
  return b;
}

std::string RefPath(const std::string& dir, const std::string& name, int f) {
  char b[16];
  std::snprintf(b, sizeof(b), "%03d", f);
  return dir + "/ref/" + name + "/" + b + ".rgba.gz";
}

bool SameIndex(const FrameIndex& a, const FrameIndex& b, std::string* why) {
  if (a.status != b.status) {
    *why = std::string("status ") + ToString(a.status) + " vs " +
           ToString(b.status);
    return false;
  }
  if (a.frames.size() != b.frames.size()) {
    *why = "frame count " + std::to_string(a.frames.size()) + " vs " +
           std::to_string(b.frames.size());
    return false;
  }
  for (std::size_t i = 0; i < a.frames.size(); ++i) {
    if (!(a.frames[i] == b.frames[i])) {
      *why = "frame " + std::to_string(i) + " differs";
      return false;
    }
  }
  if (a.status == IndexStatus::kOk || a.status == IndexStatus::kTruncated) {
    if (a.video.number != b.video.number || a.video.codec != b.video.codec ||
        a.video.width != b.video.width || a.video.height != b.video.height ||
        a.video.alpha_mode != b.video.alpha_mode ||
        a.video.default_duration_ns != b.video.default_duration_ns ||
        a.has_audio != b.has_audio ||
        a.timecode_scale_ns != b.timecode_scale_ns) {
      *why = "track info differs";
      return false;
    }
  }
  return true;
}

double Median(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}
double Pct(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(p * v.size()))];
}
double Mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? 0 : s / v.size();
}

// Microseconds to index the file, median of `reps` runs.
template <typename F>
double TimeIndexUs(F fn, const std::vector<std::uint8_t>& d, int reps) {
  std::vector<double> t;
  Limits lim;
  for (int i = 0; i < reps; ++i) {
    auto t0 = Clock::now();
    FrameIndex idx = fn(d.data(), d.size(), lim);
    t.push_back(MsSince(t0) * 1000.0);
    if (idx.frames.empty() && idx.status == IndexStatus::kOk) break;
  }
  return Median(t);
}

// ---------------------------------------------------------------------------
// verify

int Verify(const std::string& dir, const std::string& expect_path) {
  int failures = 0;
  auto fail = [&](const std::string& name, const std::string& what) {
    std::printf("FAIL %s: %s\n", name.c_str(), what.c_str());
    ++failures;
  };
  struct Row {
    std::string name;
    std::size_t frames;
    double color_mean, color_p95, alpha_mean, alpha_p95, conv_mean, total_mean,
        total_p95;
    double idx_libwebm_us, idx_mini_us;
  };
  std::vector<Row> rows;

  std::printf(
      "## Per reference frame (premultiplied RGBA vs test_assets/ref)\n\n");
  std::printf(
      "| file | frame | alpha max err | alpha PSNR | RGB max err | RGB PSNR | "
      "RGB PSNR exact-premul | RGB PSNR bilinear-UV | premultiplied |\n");
  std::printf("|---|---|---|---|---|---|---|---|---|\n");

  double min_alpha_psnr = INFINITY, min_rgb_psnr = INFINITY;
  for (const Expect& e : LoadExpect(expect_path)) {
    if (e.kind != "webm") continue;
    const std::vector<std::uint8_t> file = ReadFile(dir + "/" + e.name);
    if (file.empty()) {
      fail(e.name, "cannot read");
      continue;
    }
    Limits lim;
    const FrameIndex a = IndexWithLibwebm(file.data(), file.size(), lim);
    const FrameIndex b = IndexWithMiniEbml(file.data(), file.size(), lim);
    std::string why;
    if (!SameIndex(a, b, &why)) fail(e.name, "demuxers disagree: " + why);

    for (const FrameIndex* idx : {&a, &b}) {
      const char* who = idx == &a ? "libwebm" : "mini";
      const IndexStatus want =
          e.expect == "ok"               ? IndexStatus::kOk
          : e.expect == "truncated"      ? IndexStatus::kTruncated
          : e.expect == "limit_exceeded" ? IndexStatus::kLimit
                                         : IndexStatus::kInvalid;
      if (idx->status != want) {
        fail(e.name, std::string(who) + ": status " + ToString(idx->status) +
                         " (" + idx->error + "), expected " + e.expect);
      }
      if (want == IndexStatus::kLimit) continue;
      if (static_cast<int>(idx->frames.size()) != e.frames) {
        fail(e.name, std::string(who) + ": " +
                         std::to_string(idx->frames.size()) +
                         " frames, expected " + std::to_string(e.frames));
        continue;
      }
      for (int i = 0; i < e.frames; ++i) {
        const long ms = std::lround(idx->frames[i].pts_ns / 1e6);
        if (i < static_cast<int>(e.pts_ms.size()) && ms != e.pts_ms[i]) {
          fail(e.name,
               std::string(who) + ": pts of frame " + std::to_string(i));
          break;
        }
      }
      std::vector<int> kf;
      for (int i = 0; i < e.frames; ++i) {
        if (idx->frames[i].keyframe) kf.push_back(i);
      }
      if (kf != e.keyframes) fail(e.name, std::string(who) + ": keyframes");
      if ((idx->video.alpha_mode == 1) != e.alpha) {
        fail(e.name, std::string(who) + ": AlphaMode");
      }
      if (idx->has_audio != e.has_audio)
        fail(e.name, std::string(who) + ": audio");
      if (static_cast<int>(idx->video.width) != e.w ||
          static_cast<int>(idx->video.height) != e.h) {
        fail(e.name, std::string(who) + ": dimensions");
      }
      if ((idx->video.codec == Codec::kVp9) != (e.codec == "vp9")) {
        fail(e.name, std::string(who) + ": codec");
      }
    }
    if (e.expect == "limit_exceeded") continue;
    const FrameIndex& idx = b;

    // Every frame of an alpha track must carry alpha.
    if (idx.video.alpha_mode == 1) {
      for (std::size_t i = 0; i < idx.frames.size(); ++i) {
        if (!idx.frames[i].alpha.present()) {
          fail(e.name,
               "frame " + std::to_string(i) + " has no BlockAdditional");
        }
      }
    }

    // Decode + compare. Three merges per reference frame.
    ClipDecoder dec(file.data(), idx), dec_exact(file.data(), idx),
        dec_bilinear(file.data(), idx);
    if (!dec.Init() || !dec_exact.Init() || !dec_bilinear.Init()) {
      fail(e.name, "decoder init");
      continue;
    }
    std::vector<std::uint8_t> rgba, rgba_exact, rgba_bilinear, ref;
    for (std::size_t i = 0; i < idx.frames.size(); ++i) {
      const bool is_ref = std::find(e.ref_frames.begin(), e.ref_frames.end(),
                                    static_cast<int>(i)) != e.ref_frames.end();
      std::string err;
      if (!dec.Frame(i, is_ref ? &rgba : nullptr, Merge::kAttenuate, nullptr,
                     &err) ||
          !dec_exact.Frame(i, is_ref ? &rgba_exact : nullptr,
                           Merge::kExactPremultiply, nullptr, &err) ||
          !dec_bilinear.Frame(i, is_ref ? &rgba_bilinear : nullptr,
                              Merge::kBilinearAttenuate, nullptr, &err)) {
        fail(e.name, "decode frame " + std::to_string(i) + ": " + err);
        break;
      }
      if (!is_ref) continue;
      if (!ReadGz(RefPath(dir, e.name, static_cast<int>(i)),
                  static_cast<std::size_t>(e.w) * e.h * 4, &ref)) {
        fail(e.name, "reference " + std::to_string(i));
        continue;
      }
      const Cmp c = Compare(rgba, ref);
      const Cmp cx = Compare(rgba_exact, ref);
      const Cmp cb = Compare(rgba_bilinear, ref);
      std::printf("| %s | %zu | %d | %s | %d | %s | %s | %s | %s |\n",
                  e.name.c_str(), i, c.max_a, Db(Psnr(c.mse_a)).c_str(),
                  c.max_rgb, Db(Psnr(c.mse_rgb)).c_str(),
                  Db(Psnr(cx.mse_rgb)).c_str(), Db(Psnr(cb.mse_rgb)).c_str(),
                  c.premultiplied ? "yes" : "NO");
      min_alpha_psnr = std::min(min_alpha_psnr, Psnr(c.mse_a));
      min_rgb_psnr = std::min(min_rgb_psnr, Psnr(c.mse_rgb));
      if (Psnr(c.mse_a) < 45.0) fail(e.name, "alpha PSNR < 45 dB");
      if (!c.premultiplied) fail(e.name, "colour exceeds alpha");
    }

    // Timing: whole clip, 10 loops, fresh decoders per loop as a player
    // would have after a seek to 0.
    std::vector<double> color, alpha, conv, total;
    for (int loop = 0; loop < 10; ++loop) {
      ClipDecoder d(file.data(), idx);
      d.Init();
      for (std::size_t i = 0; i < idx.frames.size(); ++i) {
        FrameTiming t;
        std::string err;
        d.Frame(i, &rgba, Merge::kAttenuate, &t, &err);
        color.push_back(t.color_ms);
        alpha.push_back(t.alpha_ms);
        conv.push_back(t.convert_ms);
        total.push_back(t.color_ms + t.alpha_ms + t.convert_ms);
      }
    }
    rows.push_back({e.name, idx.frames.size(), Mean(color), Pct(color, 0.95),
                    Mean(alpha), Pct(alpha, 0.95), Mean(conv), Mean(total),
                    Pct(total, 0.95), TimeIndexUs(IndexWithLibwebm, file, 200),
                    TimeIndexUs(IndexWithMiniEbml, file, 200)});
  }
  // Expected failures still need both demuxers to fail the same way.
  for (const Expect& e : LoadExpect(expect_path)) {
    if (e.kind == "webm" || e.expect != "invalid") continue;
    const std::vector<std::uint8_t> file = ReadFile(dir + "/" + e.name);
    Limits lim;
    const FrameIndex a = IndexWithLibwebm(file.data(), file.size(), lim);
    const FrameIndex b = IndexWithMiniEbml(file.data(), file.size(), lim);
    std::printf("\n%s: libwebm=%s (%s), mini=%s (%s)\n", e.name.c_str(),
                ToString(a.status), a.error.c_str(), ToString(b.status),
                b.error.c_str());
    if (a.status != IndexStatus::kInvalid ||
        b.status != IndexStatus::kInvalid) {
      fail(e.name, "expected invalid from both demuxers");
    }
  }

  std::printf(
      "\n## Host timing (ms per frame, single-threaded libvpx, 10 loops)\n\n");
  std::printf(
      "| file | frames | colour mean | colour p95 | alpha mean | alpha p95 | "
      "merge mean | total mean | total p95 | index libwebm µs | index mini µs "
      "|\n");
  std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
  for (const Row& r : rows) {
    std::printf(
        "| %s | %zu | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.3f | %.1f | "
        "%.1f |\n",
        r.name.c_str(), r.frames, r.color_mean, r.color_p95, r.alpha_mean,
        r.alpha_p95, r.conv_mean, r.total_mean, r.total_p95, r.idx_libwebm_us,
        r.idx_mini_us);
  }
  std::printf("\nmin alpha PSNR: %s dB, min RGB PSNR: %s dB\n",
              Db(min_alpha_psnr).c_str(), Db(min_rgb_psnr).c_str());
  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL CHECKS PASSED",
              failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}

// ---------------------------------------------------------------------------
// experiments

// Rewrites the size of the Segment and of every Cluster as "unknown" (all
// ones, same length), the way live muxers write them.
std::vector<std::uint8_t> MakeUnknownSizes(std::vector<std::uint8_t> d) {
  auto vlen = [](std::uint8_t b) {
    int n = 1;
    while (n <= 8 && !(b & (0x80 >> (n - 1)))) ++n;
    return n;
  };
  std::size_t pos = 0;
  // Skip the EBML header.
  {
    const int il = vlen(d[pos]);
    const int sl = vlen(d[pos + il]);
    std::uint64_t sz = d[pos + il] & (0xFF >> sl);
    for (int i = 1; i < sl; ++i) sz = (sz << 8) | d[pos + il + i];
    pos += il + sl + sz;
  }
  // An all-ones value of the same length means "unknown size".
  auto set_unknown = [&](std::size_t at, int sl) {
    d[at] = static_cast<std::uint8_t>((0x80 >> (sl - 1)) | (0xFF >> sl));
    for (int i = 1; i < sl; ++i) d[at + i] = 0xFF;
  };
  // Segment header.
  const int il = vlen(d[pos]);
  const int sl = vlen(d[pos + il]);
  set_unknown(pos + il, sl);
  pos += il + sl;
  // Walk Segment children; every Cluster gets an unknown size, but we still
  // step over it using its original size (read before overwriting).
  while (pos + 2 < d.size()) {
    const int cil = vlen(d[pos]);
    std::uint32_t id = 0;
    for (int i = 0; i < cil; ++i) id = (id << 8) | d[pos + i];
    const int csl = vlen(d[pos + cil]);
    std::uint64_t sz = d[pos + cil] & (0xFF >> csl);
    for (int i = 1; i < csl; ++i) sz = (sz << 8) | d[pos + cil + i];
    if (id == 0x1F43B675) set_unknown(pos + cil, csl);
    pos += cil + csl + sz;
  }
  return d;
}

// The flags byte of an unlaced block sits right before its frame data.
std::size_t FirstBlockFlagsOffset(const FrameIndex& idx) {
  return static_cast<std::size_t>(idx.frames[0].data.offset - 1);
}

// Runs fn in a child process; returns a description of how it ended and the
// child's peak RSS growth.
template <typename F>
std::string InChild(F fn, long* maxrss_kb) {
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    const int rc = fn();
    std::fflush(stdout);
    _exit(rc);
  }
  int st = 0;
  struct rusage ru;
  wait4(pid, &st, 0, &ru);
#ifdef __APPLE__
  *maxrss_kb = ru.ru_maxrss / 1024;  // Bytes on macOS.
#else
  *maxrss_kb = ru.ru_maxrss;
#endif
  if (WIFEXITED(st)) return "exit " + std::to_string(WEXITSTATUS(st));
  if (WIFSIGNALED(st))
    return "killed by signal " + std::to_string(WTERMSIG(st));
  return "?";
}

void PutVint(std::vector<std::uint8_t>* o, std::uint64_t v, int len) {
  for (int i = len - 1; i >= 0; --i) {
    std::uint8_t b = static_cast<std::uint8_t>(v >> (8 * i));
    if (i == len - 1) b |= static_cast<std::uint8_t>(0x80 >> (len - 1));
    o->push_back(b);
  }
}
void PutId(std::vector<std::uint8_t>* o, std::uint32_t id) {
  int n = id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
  for (int i = n - 1; i >= 0; --i)
    o->push_back(static_cast<std::uint8_t>(id >> (8 * i)));
}
void PutUnknown(std::vector<std::uint8_t>* o) {
  o->push_back(0x01);
  for (int i = 0; i < 7; ++i) o->push_back(0xFF);
}
// Element with a known size and the given body.
void PutElem(std::vector<std::uint8_t>* o, std::uint32_t id,
             const std::vector<std::uint8_t>& body) {
  PutId(o, id);
  PutVint(o, body.size(), 8);
  o->insert(o->end(), body.begin(), body.end());
}
std::vector<std::uint8_t> U8(std::uint64_t v) {
  std::vector<std::uint8_t> b;
  for (int i = 7; i >= 0; --i)
    b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  return b;
}
std::vector<std::uint8_t> Str(const char* s) {
  return std::vector<std::uint8_t>(s, s + std::strlen(s));
}

// EBML header + unknown-size Segment + Tracks (VP9 track 1, 64x64, alpha).
std::vector<std::uint8_t> HostilePrefix() {
  std::vector<std::uint8_t> o, ebml, track, video, tracks;
  PutElem(&ebml, 0x4282, Str("webm"));
  PutElem(&o, 0x1A45DFA3, ebml);
  PutId(&o, 0x18538067);
  PutUnknown(&o);
  PutElem(&video, 0xB0, U8(64));
  PutElem(&video, 0xBA, U8(64));
  PutElem(&video, 0x53C0, U8(1));
  PutElem(&track, 0xD7, U8(1));
  PutElem(&track, 0x83, U8(1));
  PutElem(&track, 0x86, Str("V_VP9"));
  PutElem(&track, 0xE0, video);
  PutElem(&tracks, 0xAE, track);
  PutElem(&o, 0x1654AE6B, tracks);
  PutId(&o, 0x1F43B675);  // Cluster, unknown size.
  PutUnknown(&o);
  PutElem(&o, 0xE7, U8(0));  // Timecode.
  return o;
}

int Experiments(const std::string& dir) {
  Limits lim;
  auto load = [&](const char* n) { return ReadFile(dir + "/" + n); };

  // E1: alpha vs colour keyframes, and seeking to a colour keyframe.
  std::printf("## E1 keyframe alignment (VP8/VP9 peek on each payload)\n\n");
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm",
                           "vp9_alpha_vfr.webm", "vp9_odd_301x199.webm",
                           "vp9_with_audio.webm", "vp9_single_frame.webm"}) {
    const auto f = load(name);
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    std::string ck, ak;
    int mismatch = 0;
    for (std::size_t i = 0; i < idx.frames.size(); ++i) {
      const FrameEntry& fr = idx.frames[i];
      const bool c = PeekKeyframe(idx.video.codec, f.data() + fr.data.offset,
                                  fr.data.size);
      const bool a = fr.alpha.present() &&
                     PeekKeyframe(idx.video.codec, f.data() + fr.alpha.offset,
                                  fr.alpha.size);
      if (c) ck += std::to_string(i) + " ";
      if (a) ak += std::to_string(i) + " ";
      if (fr.alpha.present() && c != a) ++mismatch;
      if (c != fr.keyframe) {
        std::printf(
            "  %s frame %zu: container keyframe flag %d, bitstream %d\n", name,
            i, fr.keyframe, c);
      }
    }
    std::printf("- %s: colour kf [%s], alpha kf [%s], mismatches %d\n", name,
                ck.c_str(), ak.c_str(), mismatch);
  }

  // E1b: random access. Decode from the mid keyframe on fresh decoders and
  // compare against the reference.
  {
    const auto f = load("vp9_alpha_512.webm");
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    ClipDecoder d(f.data(), idx);
    d.Init();
    std::vector<std::uint8_t> rgba, ref;
    std::string err;
    for (std::size_t i = 45; i < idx.frames.size(); ++i) {
      if (!d.Frame(i, &rgba, Merge::kAttenuate, nullptr, &err)) {
        std::printf("- seek to 45 failed at %zu: %s\n", i, err.c_str());
        break;
      }
      if (i == 45 || i == 89) {
        ReadGz(RefPath(dir, "vp9_alpha_512.webm", static_cast<int>(i)),
               rgba.size(), &ref);
        const Cmp c = Compare(rgba, ref);
        std::printf("- seek to kf 45, frame %zu: alpha %s dB, RGB %s dB\n", i,
                    Db(Psnr(c.mse_a)).c_str(), Db(Psnr(c.mse_rgb)).c_str());
      }
    }
  }

  // E1c: looping. Decode the clip twice on the same decoders; the second
  // pass must match the references exactly as well as the first.
  {
    const auto f = load("vp8_alpha_256.webm");
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    ClipDecoder d(f.data(), idx);
    d.Init();
    std::vector<std::uint8_t> rgba, ref;
    std::string err;
    double worst = INFINITY;
    for (int loop = 0; loop < 2; ++loop) {
      for (std::size_t i = 0; i < idx.frames.size(); ++i) {
        d.Frame(i, &rgba, Merge::kAttenuate, nullptr, &err);
        if (loop == 1 && (i == 29 || i == 59)) {
          ReadGz(RefPath(dir, "vp8_alpha_256.webm", static_cast<int>(i)),
                 rgba.size(), &ref);
          worst = std::min(worst, Psnr(Compare(rgba, ref).mse_a));
        }
      }
    }
    std::printf(
        "- loop twice on one decoder pair (vp8_alpha_256): 2nd-pass alpha %s "
        "dB\n",
        Db(worst).c_str());
  }

  // E2: a frame without its BlockAdditional in the middle of a GOP.
  std::printf(
      "\n## E2 missing alpha on frame 10 of vp9_alpha_512 (naive: keep "
      "decoding)\n\n");
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm"}) {
    const auto f = load(name);
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    ClipDecoder d(f.data(), idx);
    d.Init();
    std::vector<std::uint8_t> rgba, ref;
    std::string err;
    const int probe = std::string(name).find("vp8") == 0 ? 29 : 44;
    for (int i = 0; i <= probe; ++i) {
      if (!d.Frame(i, &rgba, Merge::kAttenuate, nullptr, &err, i == 10)) {
        std::printf("- %s: decode error at %d: %s\n", name, i, err.c_str());
        break;
      }
      if (i == probe) {
        ReadGz(RefPath(dir, name, i), rgba.size(), &ref);
        const Cmp c = Compare(rgba, ref);
        std::printf(
            "- %s: no libvpx error; frame %d alpha %s dB (max err %d) vs ref\n",
            name, i, Db(Psnr(c.mse_a)).c_str(), c.max_a);
      }
    }
  }

  // E3: laced video block.
  std::printf("\n## E3 laced video block (Xiph lacing bit set on frame 0)\n\n");
  for (const char* name : {"vp9_opaque_256.webm", "vp9_alpha_512.webm"}) {
    auto f = load(name);
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    f[FirstBlockFlagsOffset(idx)] |= 0x02;
    const FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
    const FrameIndex b = IndexWithMiniEbml(f.data(), f.size(), lim);
    std::printf("- %s: libwebm=%s (%s), mini=%s (%s)\n", name,
                ToString(a.status), a.error.c_str(), ToString(b.status),
                b.error.c_str());
  }

  // E4: unknown-size Segment and Clusters (live-muxer style).
  std::printf("\n## E4 unknown-size Segment + Clusters\n\n");
  for (const char* name : {"vp9_alpha_512.webm", "vp8_alpha_256.webm",
                           "vp9_with_audio.webm", "truncated.webm"}) {
    const auto orig = load(name);
    const auto f = MakeUnknownSizes(orig);
    const FrameIndex o = IndexWithMiniEbml(orig.data(), orig.size(), lim);
    const FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
    const FrameIndex b = IndexWithMiniEbml(f.data(), f.size(), lim);
    std::string why_a, why_b;
    const bool sa =
        SameIndex(o, a, &why_a) || (o.status == IndexStatus::kTruncated &&
                                    o.frames.size() == a.frames.size());
    const bool sb =
        SameIndex(o, b, &why_b) || (o.status == IndexStatus::kTruncated &&
                                    o.frames.size() == b.frames.size());
    std::printf("- %s: libwebm %s, %zu frames%s; mini %s, %zu frames%s\n", name,
                ToString(a.status), a.frames.size(),
                sa ? " (same as original)" : (" DIFFERENT: " + why_a).c_str(),
                ToString(b.status), b.frames.size(),
                sb ? " (same as original)" : (" DIFFERENT: " + why_b).c_str());
  }

  // E5: hostile sizes. A leaf that claims 2^55 bytes inside an unknown-size
  // Segment, so no parent bounds it. Info/Title is used because our callback
  // never reads BlockAdditional bodies (it skips them and keeps the span).
  std::printf(
      "\n## E5 hostile leaf size (Info/Title of 2^55 bytes in a 76-byte "
      "file)\n\n");
  {
    std::vector<std::uint8_t> f, ebml;
    PutElem(&ebml, 0x4282, Str("webm"));
    PutElem(&f, 0x1A45DFA3, ebml);
    PutId(&f, 0x18538067);
    PutUnknown(&f);
    const std::uint64_t huge = 1ull << 55;
    PutId(&f, 0x1549A966);      // Info
    PutVint(&f, huge + 10, 8);  // Exactly holds the Title.
    PutId(&f, 0x7BA9);          // Title
    PutVint(&f, huge, 8);
    for (int i = 0; i < 16; ++i) f.push_back('x');
    long rss = 0;
    std::string how = InChild(
        [&] {
          g_libwebm_unguarded = true;
          FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
          std::printf("  unguarded libwebm returned %s (%s)\n",
                      ToString(a.status), a.error.c_str());
          return 0;
        },
        &rss);
    std::printf(
        "- libwebm without our OnElementBegin cap: %s, peak RSS %ld KiB\n",
        how.c_str(), rss);
    how = InChild(
        [&] {
          FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
          std::printf("  guarded libwebm: %s (%s)\n", ToString(a.status),
                      a.error.c_str());
          return 0;
        },
        &rss);
    std::printf("- libwebm with the cap: %s, peak RSS %ld KiB\n", how.c_str(),
                rss);
    const FrameIndex b = IndexWithMiniEbml(f.data(), f.size(), lim);
    std::printf("- mini: %s (%s)\n", ToString(b.status), b.error.c_str());
  }

  // E6: DOM amplification. 2 MiB of empty BlockGroups in one Cluster.
  std::printf(
      "\n## E6 2 MiB of empty BlockGroups (A0 80) in one unknown-size "
      "Cluster\n\n");
  std::printf(
      "sizeof(webm::Element<webm::BlockGroup>) = %zu, "
      "sizeof(webm::Element<webm::SimpleBlock>) = %zu\n\n",
      sizeof(webm::Element<webm::BlockGroup>),
      sizeof(webm::Element<webm::SimpleBlock>));
  {
    std::vector<std::uint8_t> f = HostilePrefix();
    while (f.size() < (2u << 20)) {
      f.push_back(0xA0);
      f.push_back(0x80);
    }
    long rss = 0;
    for (int guarded = 0; guarded < 2; ++guarded) {
      std::string how = InChild(
          [&] {
            g_libwebm_unguarded = !guarded;
            auto t0 = Clock::now();
            FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
            std::printf("  libwebm %s: %s (%s) in %.0f ms\n",
                        guarded ? "guarded" : "unguarded", ToString(a.status),
                        a.error.c_str(), MsSince(t0));
            return 0;
          },
          &rss);
      std::printf("- libwebm %s: %s, peak RSS %ld KiB\n",
                  guarded ? "with element cap" : "without element cap",
                  how.c_str(), rss);
    }
    std::string how = InChild(
        [&] {
          auto t0 = Clock::now();
          FrameIndex b = IndexWithMiniEbml(f.data(), f.size(), lim);
          std::printf("  mini: %s (%s), %zu frames in %.0f ms\n",
                      ToString(b.status), b.error.c_str(), b.frames.size(),
                      MsSince(t0));
          return 0;
        },
        &rss);
    std::printf("- mini: %s, peak RSS %ld KiB\n", how.c_str(), rss);
  }

  // E7: over-limit stream reaching libvpx directly (demuxer limit bypassed).
  std::printf(
      "\n## E7 vp9_2048.webm straight into libvpx "
      "(--size-limit=1024x1024)\n\n");
  {
    const auto f = load("vp9_2048.webm");
    Limits big;
    big.max_dimension = 4096;
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), big);
    VpxDecoder d;
    d.Init(idx.video.codec);
    int shown = 0;
    std::string err;
    const vpx_image_t* img = d.Decode(f.data() + idx.frames[0].data.offset,
                                      idx.frames[0].data.size, &shown, &err);
    std::printf("- demuxer with default limits: %s; libvpx: %s\n",
                ToString(IndexWithMiniEbml(f.data(), f.size(), lim).status),
                img ? "decoded (size limit NOT enforced)" : err.c_str());
  }

  // E8: alpha stream whose size differs from the colour stream.
  std::printf("\n## E8 alpha plane size != colour size\n\n");
  {
    const auto c = load("vp9_alpha_512.webm");
    const auto a = load("vp9_alpha_vfr.webm");
    const FrameIndex ci = IndexWithMiniEbml(c.data(), c.size(), lim);
    const FrameIndex ai = IndexWithMiniEbml(a.data(), a.size(), lim);
    VpxDecoder dc, da;
    dc.Init(Codec::kVp9);
    da.Init(Codec::kVp9);
    int shown = 0;
    std::string err;
    const vpx_image_t* ci0 = dc.Decode(c.data() + ci.frames[0].data.offset,
                                       ci.frames[0].data.size, &shown, &err);
    const vpx_image_t* ai0 = da.Decode(a.data() + ai.frames[0].alpha.offset,
                                       ai.frames[0].alpha.size, &shown, &err);
    std::vector<std::uint8_t> out(512 * 512 * 4);
    const bool ok =
        ToRgba(ci0, ai0, Merge::kAttenuate, out.data(), 512 * 4, &err);
    std::printf("- 512x512 colour + 192x192 alpha: merge %s (%s)\n",
                ok ? "ACCEPTED" : "rejected", err.c_str());
  }

  // E9: colour space and range signalled by the bitstreams.
  std::printf("\n## E9 colour space / range reported by libvpx\n\n");
  for (const char* name :
       {"vp9_alpha_512.webm", "vp8_alpha_256.webm", "vp9_opaque_256.webm"}) {
    const auto f = load(name);
    const FrameIndex idx = IndexWithMiniEbml(f.data(), f.size(), lim);
    VpxDecoder d;
    d.Init(idx.video.codec);
    int shown = 0;
    std::string err;
    const vpx_image_t* img = d.Decode(f.data() + idx.frames[0].data.offset,
                                      idx.frames[0].data.size, &shown, &err);
    std::printf("- %s: cs=%d range=%d fmt=0x%x\n", name, img ? img->cs : -1,
                img ? img->range : -1, img ? img->fmt : 0);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// stress

int Stress(const std::string& dir, long iterations, unsigned seed) {
  std::vector<std::vector<std::uint8_t>> corpus;
  for (const char* n :
       {"vp9_alpha_512.webm", "vp9_opaque_256.webm", "vp8_alpha_256.webm",
        "vp9_alpha_vfr.webm", "vp9_odd_301x199.webm", "vp9_single_frame.webm",
        "vp9_with_audio.webm", "vp9_2048.webm", "truncated.webm"}) {
    corpus.push_back(ReadFile(dir + "/" + n));
    corpus.push_back(MakeUnknownSizes(corpus.back()));
  }
  std::mt19937 rng(seed);
  auto rnd = [&](std::size_t n) {
    return n == 0 ? 0 : static_cast<std::size_t>(rng() % n);
  };
  Limits lim;
  lim.max_frames = 200;
  long counts[2][5] = {};
  long agree = 0, identical = 0, decoded_frames = 0, decode_errors = 0;
  std::map<std::string, long> diffs;
  double t_lib = 0, t_mini = 0;
  for (long it = 0; it < iterations; ++it) {
    std::vector<std::uint8_t> f = corpus[rnd(corpus.size())];
    const int muts = 1 + static_cast<int>(rnd(4));
    for (int m = 0; m < muts && !f.empty(); ++m) {
      switch (rnd(6)) {
        case 0:  // Flip a few bits.
          for (int k = 0; k < 4; ++k) f[rnd(f.size())] ^= 1u << rnd(8);
          break;
        case 1:  // Truncate.
          f.resize(rnd(f.size()) + 1);
          break;
        case 2: {  // Overwrite a short run with 0xFF (unknown sizes, huge ids).
          const std::size_t p = rnd(f.size());
          for (std::size_t k = p; k < std::min(f.size(), p + 1 + rnd(8)); ++k)
            f[k] = 0xFF;
          break;
        }
        case 3: {  // Random byte that looks like a vint length marker.
          f[rnd(f.size())] = static_cast<std::uint8_t>(0x80 >> rnd(8)) |
                             static_cast<std::uint8_t>(rnd(2));
          break;
        }
        case 4: {  // Duplicate a chunk.
          const std::size_t p = rnd(f.size()),
                            n = rnd(std::min<std::size_t>(512, f.size() - p));
          std::vector<std::uint8_t> chunk(f.begin() + p, f.begin() + p + n);
          f.insert(f.begin() + rnd(f.size()), chunk.begin(), chunk.end());
          break;
        }
        case 5: {  // Delete a chunk.
          const std::size_t p = rnd(f.size()),
                            n = rnd(std::min<std::size_t>(512, f.size() - p));
          f.erase(f.begin() + p, f.begin() + p + n);
          break;
        }
      }
    }
    auto t0 = Clock::now();
    const FrameIndex a = IndexWithLibwebm(f.data(), f.size(), lim);
    t_lib += MsSince(t0);
    t0 = Clock::now();
    const FrameIndex b = IndexWithMiniEbml(f.data(), f.size(), lim);
    t_mini += MsSince(t0);
    counts[0][static_cast<int>(a.status)]++;
    counts[1][static_cast<int>(b.status)]++;
    if (a.frames.size() == b.frames.size()) {
      ++agree;
      if (a.frames == b.frames) ++identical;
    } else {
      const std::string key =
          std::string(ToString(a.status)) + ":" + a.error + " | " +
          ToString(b.status) + ":" + b.error +
          (a.frames.size() < b.frames.size() ? " | mini more"
                                             : " | libwebm more");
      ++diffs[key];
    }

    // Decode up to 8 frames of whatever the mini index found, with both
    // contexts, to exercise libvpx + merge on mangled payloads.
    for (const FrameIndex* idx : {&a, &b}) {
      if (!idx->has_video || idx->frames.empty()) continue;
      for (const FrameEntry& fr : idx->frames) {
        if (fr.data.offset + fr.data.size > f.size() ||
            fr.alpha.offset + fr.alpha.size > f.size()) {
          std::printf("BUG: span outside input (%s)\n",
                      idx == &a ? "libwebm" : "mini");
          return 1;
        }
      }
      ClipDecoder d(f.data(), *idx);
      if (!d.Init()) continue;
      std::vector<std::uint8_t> rgba;
      for (std::size_t i = 0; i < std::min<std::size_t>(8, idx->frames.size());
           ++i) {
        std::string err;
        if (d.Frame(i, &rgba, Merge::kAttenuate, nullptr, &err)) {
          ++decoded_frames;
        } else {
          ++decode_errors;
        }
      }
    }
  }
  const char* names[] = {"ok", "truncated", "invalid", "unsupported", "limit"};
  std::printf("stress: %ld iterations, seed %u\n", iterations, seed);
  for (int d = 0; d < 2; ++d) {
    std::printf("  %-8s", d == 0 ? "libwebm" : "mini");
    for (int s = 0; s < 5; ++s) std::printf(" %s=%ld", names[s], counts[d][s]);
    std::printf("  (%.1f µs/iter)\n",
                1000.0 * (d == 0 ? t_lib : t_mini) / iterations);
  }
  std::printf(
      "  same frame count: %ld/%ld (identical index: %ld); decoded %ld "
      "frames, %ld decode errors\n",
      agree, iterations, identical, decoded_frames, decode_errors);
  std::printf(
      "  frame-count disagreements (libwebm status:error | mini "
      "status:error):\n");
  for (const auto& kv : diffs) {
    std::printf("    %5ld  %s\n", kv.second, kv.first.c_str());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// compare: every frame against a raw straight-RGBA dump from ffmpeg+libvpx.

int CompareAll(const std::string& webm, const std::string& raw) {
  const std::vector<std::uint8_t> file = ReadFile(webm);
  const std::vector<std::uint8_t> ref_all = ReadFile(raw);
  Limits lim;
  const FrameIndex a = IndexWithLibwebm(file.data(), file.size(), lim);
  const FrameIndex idx = IndexWithMiniEbml(file.data(), file.size(), lim);
  std::string why;
  const bool same = SameIndex(a, idx, &why);
  if (idx.status != IndexStatus::kOk && idx.status != IndexStatus::kTruncated) {
    std::printf("%s: index %s (%s)\n", webm.c_str(), ToString(idx.status),
                idx.error.c_str());
    return 1;
  }
  const std::size_t fs =
      static_cast<std::size_t>(idx.video.width) * idx.video.height * 4;
  const std::size_t ref_frames = fs ? ref_all.size() / fs : 0;
  std::size_t with_alpha = 0;
  for (const FrameEntry& f : idx.frames) with_alpha += f.alpha.present();
  ClipDecoder dec(file.data(), idx);
  dec.Init();
  std::vector<std::uint8_t> rgba, ref(fs);
  double min_a = INFINITY, min_rgb = INFINITY;
  int max_a = 0;
  std::size_t exact = 0, n = 0;
  std::string err;
  for (std::size_t i = 0; i < idx.frames.size() && i < ref_frames; ++i) {
    if (!dec.Frame(i, &rgba, Merge::kAttenuate, nullptr, &err)) {
      std::printf("  decode error at frame %zu: %s\n", i, err.c_str());
      break;
    }
    // Exact premultiply of ffmpeg's straight RGBA, as gen_corpus.sh does.
    const std::uint8_t* src = &ref_all[i * fs];
    for (std::size_t p = 0; p < fs; p += 4) {
      for (int k = 0; k < 3; ++k)
        ref[p + k] = (src[p + k] * src[p + 3] + 127) / 255;
      ref[p + 3] = src[p + 3];
    }
    const Cmp c = Compare(rgba, ref);
    min_a = std::min(min_a, Psnr(c.mse_a));
    min_rgb = std::min(min_rgb, Psnr(c.mse_rgb));
    max_a = std::max(max_a, c.max_a);
    exact += c.max_a == 0;
    ++n;
  }
  std::printf(
      "| %s | %s | %ux%u | %d | %zu/%zu | %zu | %zu/%zu | %d | %s | %s | %s "
      "|\n",
      webm.substr(webm.find_last_of('/') + 1).c_str(),
      idx.video.codec == Codec::kVp9 ? "vp9" : "vp8", idx.video.width,
      idx.video.height, static_cast<int>(idx.video.alpha_mode),
      idx.frames.size(), ref_frames, with_alpha, exact, n, max_a,
      Db(min_a).c_str(), Db(min_rgb).c_str(),
      same ? "yes" : ("NO: " + why).c_str());
  return (n == idx.frames.size() && n == ref_frames && max_a == 0) ? 0 : 1;
}

}  // namespace
}  // namespace spike

int main(int argc, char** argv) {
  const std::string cmd = argc > 1 ? argv[1] : "";
  if (cmd == "verify" && argc == 4) return spike::Verify(argv[2], argv[3]);
  if (cmd == "experiments" && argc == 3) return spike::Experiments(argv[2]);
  if (cmd == "compare" && argc == 4) return spike::CompareAll(argv[2], argv[3]);
  if (cmd == "stress" && argc >= 4) {
    return spike::Stress(
        argv[2], std::atol(argv[3]),
        argc > 4 ? static_cast<unsigned>(std::atol(argv[4])) : 1);
  }
  std::fprintf(stderr,
               "usage: %s verify <test_assets> <expect.txt>\n"
               "       %s experiments <test_assets>\n"
               "       %s compare <file.webm> <ffmpeg straight rgba dump>\n"
               "       %s stress <test_assets> <iterations> [seed]\n",
               argv[0], argv[0], argv[0], argv[0]);
  return 2;
}
