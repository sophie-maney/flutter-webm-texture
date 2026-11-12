// Tests for the WebM demuxer (src/demux/webm.*): the frame index of every
// corpus WebM against test_assets/manifest.json, truncation, unknown sizes,
// and hand-built files for each rule the demuxer enforces.
#include "demux/webm.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "convert/yuva.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

Bytes ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
}

wt::ByteView View(const Bytes& b) { return wt::ByteView{b.data(), b.size()}; }

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

wt_status Demux(const Bytes& b, wt::WebmIndex* index,
                const wt_limits& limits = DefaultLimits()) {
  return wt::DemuxWebm(View(b), limits, index);
}

// Rounds to the nearest millisecond, as manifest.json does.
int64_t Ms(int64_t ns) {
  return ns >= 0 ? (ns + 500000) / 1000000 : -((-ns + 500000) / 1000000);
}

bool SameFrame(const wt::WebmFrame& a, const wt::WebmFrame& b) {
  return a.data.offset == b.data.offset && a.data.size == b.data.size &&
         a.alpha.offset == b.alpha.offset && a.alpha.size == b.alpha.size &&
         a.pts_ns == b.pts_ns && a.keyframe == b.keyframe &&
         a.random_access == b.random_access;
}

// Every span lies inside the input, and every duration is positive.
void ExpectSpansInBounds(const wt::WebmIndex& index, size_t size) {
  for (size_t i = 0; i < index.frames.size(); ++i) {
    const wt::WebmFrame& f = index.frames[i];
    EXPECT_GT(f.data.size, 0u) << "frame " << i;
    EXPECT_LE(f.data.offset + f.data.size, size) << "frame " << i;
    EXPECT_LE(f.alpha.offset + f.alpha.size, size) << "frame " << i;
    EXPECT_GT(f.duration_ns, 0) << "frame " << i;
  }
}

// --- EBML builder ----------------------------------------------------------

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
  return out;
}

Bytes IdBytes(uint32_t id) {
  Bytes out;
  bool started = false;
  for (int shift = 24; shift >= 0; shift -= 8) {
    const uint8_t b = static_cast<uint8_t>(id >> shift);
    if (b != 0 || started) {
      out.push_back(b);
      started = true;
    }
  }
  return out;
}

// An 8-byte size vint.
Bytes SizeBytes(uint64_t n) {
  Bytes out = {0x01};
  for (int shift = 48; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(n >> shift));
  }
  return out;
}

const Bytes kUnknownSize = {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Bytes El(uint32_t id, const Bytes& body) {
  return Cat({IdBytes(id), SizeBytes(body.size()), body});
}

Bytes UnknownEl(uint32_t id, const Bytes& body) {
  return Cat({IdBytes(id), kUnknownSize, body});
}

Bytes U(uint32_t id, uint64_t v) {
  Bytes body;
  do {
    body.insert(body.begin(), static_cast<uint8_t>(v));
    v >>= 8;
  } while (v != 0);
  return El(id, body);
}

Bytes S(uint32_t id, const std::string& s) {
  return El(id, Bytes(s.begin(), s.end()));
}

enum : uint32_t {
  kEbml = 0x1A45DFA3,
  kDocType = 0x4282,
  kSegment = 0x18538067,
  kInfo = 0x1549A966,
  kTimecodeScale = 0x2AD7B1,
  kTracks = 0x1654AE6B,
  kTrackEntry = 0xAE,
  kTrackNumber = 0xD7,
  kTrackType = 0x83,
  kCodecId = 0x86,
  kDefaultDuration = 0x23E383,
  kContentEncodings = 0x6D80,
  kVideo = 0xE0,
  kPixelWidth = 0xB0,
  kPixelHeight = 0xBA,
  kAlphaMode = 0x53C0,
  kCluster = 0x1F43B675,
  kTimecode = 0xE7,
  kSimpleBlock = 0xA3,
  kBlockGroup = 0xA0,
  kBlock = 0xA1,
  kBlockDuration = 0x9B,
  kBlockAdditions = 0x75A1,
  kBlockMore = 0xA6,
  kBlockAddId = 0xEE,
  kBlockAdditional = 0xA5,
  kReferenceBlock = 0xFB,
  kCues = 0x1C53BB6B,
};

const Bytes kPayload = {0x10, 0x20, 0x30, 0x40};

Bytes EbmlHeader(const std::string& doctype = "webm") {
  return El(kEbml, S(kDocType, doctype));
}

Bytes VideoTrack(uint64_t number, const std::string& codec = "V_VP9",
                 uint64_t w = 64, uint64_t h = 48, uint64_t alpha_mode = 1,
                 const Bytes& extra = {}) {
  return El(kTrackEntry,
            Cat({U(kTrackNumber, number), U(kTrackType, 1), S(kCodecId, codec),
                 El(kVideo, Cat({U(kPixelWidth, w), U(kPixelHeight, h),
                                 U(kAlphaMode, alpha_mode)})),
                 extra}));
}

Bytes AudioTrack(uint64_t number) {
  return El(kTrackEntry, Cat({U(kTrackNumber, number), U(kTrackType, 2),
                              S(kCodecId, "A_OPUS")}));
}

Bytes BlockBody(uint64_t track, int16_t rel, uint8_t flags,
                const Bytes& payload = kPayload) {
  const uint16_t r = static_cast<uint16_t>(rel);
  return Cat({{static_cast<uint8_t>(0x80 | track), static_cast<uint8_t>(r >> 8),
               static_cast<uint8_t>(r), flags},
              payload});
}

Bytes SimpleBlock(uint64_t track, int16_t rel, bool key = true,
                  const Bytes& payload = kPayload) {
  return El(kSimpleBlock,
            BlockBody(track, rel, key ? uint8_t{0x80} : uint8_t{0}, payload));
}

Bytes AlphaAdditions(const Bytes& alpha, int64_t add_id = -1) {
  Bytes more =
      add_id >= 0 ? U(kBlockAddId, static_cast<uint64_t>(add_id)) : Bytes{};
  more = Cat({more, El(kBlockAdditional, alpha)});
  return El(kBlockAdditions, El(kBlockMore, more));
}

Bytes BlockGroup(uint64_t track, int16_t rel, const Bytes& extra = {},
                 const Bytes& payload = kPayload) {
  return El(kBlockGroup,
            Cat({El(kBlock, BlockBody(track, rel, 0, payload)), extra}));
}

Bytes Cluster(uint64_t tc, const Bytes& blocks) {
  return El(kCluster, Cat({U(kTimecode, tc), blocks}));
}

// EBML header + Segment{Tracks{tracks}, clusters}.
Bytes File(const Bytes& tracks, const Bytes& clusters,
           const Bytes& before_tracks = {}) {
  return Cat(
      {EbmlHeader(),
       El(kSegment, Cat({before_tracks, El(kTracks, tracks), clusters}))});
}

Bytes OneFrameFile() {
  return File(VideoTrack(1), Cluster(0, SimpleBlock(1, 0)));
}

// --- Corpus ----------------------------------------------------------------

wt_status ExpectedStatus(const std::string& expect) {
  if (expect == "ok") return WT_OK;
  if (expect == "truncated") return WT_ERR_TRUNCATED;
  if (expect == "limit_exceeded") return WT_ERR_LIMIT;
  return WT_ERR_INVALID_DATA;
}

// Compares an index with a manifest row's metadata and per-frame lists.
void ExpectMatchesManifest(const wt::WebmIndex& index, const wt_test::Json& row,
                           size_t file_size) {
  const std::string codec = row["codec"].string;
  EXPECT_EQ(index.video.codec, codec == "vp8" ? WT_CODEC_VP8 : WT_CODEC_VP9);
  EXPECT_EQ(index.video.width, row["width"].Int());
  EXPECT_EQ(index.video.height, row["height"].Int());
  EXPECT_EQ(index.video.has_alpha(), row["alpha"].boolean);
  EXPECT_EQ(index.has_audio, row["has_audio"].boolean);
  ASSERT_EQ(index.frames.size(), static_cast<size_t>(row["frame_count"].Int()));

  std::vector<int64_t> pts, durations, keyframes, random_access;
  for (size_t i = 0; i < index.frames.size(); ++i) {
    const wt::WebmFrame& f = index.frames[i];
    pts.push_back(Ms(f.pts_ns));
    durations.push_back(Ms(f.duration_ns));
    if (f.keyframe) keyframes.push_back(static_cast<int64_t>(i));
    if (f.random_access) random_access.push_back(static_cast<int64_t>(i));
    // Every frame of an alpha track carries alpha in the corpus.
    EXPECT_EQ(!f.alpha.empty(), row["alpha"].boolean) << "frame " << i;
  }
  EXPECT_EQ(pts, row["pts_ms"].Ints());
  EXPECT_EQ(durations, row["durations_ms"].Ints());
  EXPECT_EQ(keyframes, row["keyframes"].Ints());
  // Alpha keyframes line up with colour keyframes in the corpus (spike A, E1).
  EXPECT_EQ(random_access, row["keyframes"].Ints());
  ExpectSpansInBounds(index, file_size);
}

TEST(WebmCorpusTest, EveryWebmMatchesManifest) {
  const wt_test::Json& files = wt_test::Manifest()["files"];
  ASSERT_FALSE(files.array.empty()) << "manifest.json missing or unparsable";
  int webms = 0;
  for (const wt_test::Json& row : files.array) {
    const std::string name = row["name"].string;
    if (row["kind"].string != "webm") continue;
    ++webms;
    SCOPED_TRACE(name);
    const Bytes bytes = ReadAll(kAssets / name);
    ASSERT_EQ(bytes.size(), static_cast<size_t>(row["bytes"].Int()));
    wt::WebmIndex index;
    const wt_status expected = ExpectedStatus(row["expect"].string);
    EXPECT_EQ(Demux(bytes, &index), expected) << index.error;
    EXPECT_EQ(index.status, expected);
    if (expected == WT_ERR_LIMIT) {
      EXPECT_TRUE(index.frames.empty());
      // Raising the limit indexes it, which checks its manifest row too.
      wt_limits loose = DefaultLimits();
      loose.max_dimension = 4096;
      ASSERT_EQ(Demux(bytes, &index, loose), WT_OK) << index.error;
    }
    ExpectMatchesManifest(index, row, bytes.size());
  }
  EXPECT_GE(webms, 9);
}

TEST(WebmCorpusTest, OtherFormatsAreInvalid) {
  for (const char* name : {"garbage.bin", "anim.webp", "anim_disposal.gif"}) {
    wt::WebmIndex index;
    EXPECT_EQ(Demux(ReadAll(kAssets / name), &index), WT_ERR_INVALID_DATA)
        << name;
    EXPECT_TRUE(index.frames.empty());
  }
}

TEST(WebmCorpusTest, TruncatedIsAPrefixOfItsSource) {
  const Bytes full = ReadAll(kAssets / "vp9_alpha_512.webm");
  const Bytes cut = ReadAll(kAssets / "truncated.webm");
  ASSERT_LT(cut.size(), full.size());
  ASSERT_TRUE(std::equal(cut.begin(), cut.end(), full.begin()));
  wt::WebmIndex a, b;
  ASSERT_EQ(Demux(full, &a), WT_OK);
  ASSERT_EQ(Demux(cut, &b), WT_ERR_TRUNCATED);
  EXPECT_STRNE(b.error, "");
  ASSERT_EQ(b.frames.size(), 60u);
  for (size_t i = 0; i < b.frames.size(); ++i) {
    EXPECT_TRUE(SameFrame(a.frames[i], b.frames[i])) << "frame " << i;
  }
}

// Every proper prefix of a file is truncated and indexes a prefix of its
// frames, never more and never different ones.
void CheckEveryPrefix(const char* name) {
  SCOPED_TRACE(name);
  const Bytes full = ReadAll(kAssets / name);
  wt::WebmIndex whole;
  ASSERT_EQ(Demux(full, &whole), WT_OK);
  size_t last_count = 0;
  for (size_t n = 0; n < full.size(); ++n) {
    const Bytes prefix(full.begin(), full.begin() + static_cast<long>(n));
    wt::WebmIndex index;
    ASSERT_EQ(Demux(prefix, &index), WT_ERR_TRUNCATED)
        << "prefix " << n << ": " << index.error;
    ASSERT_GE(index.frames.size(), last_count) << "prefix " << n;
    ASSERT_LE(index.frames.size(), whole.frames.size()) << "prefix " << n;
    for (size_t i = 0; i < index.frames.size(); ++i) {
      ASSERT_TRUE(SameFrame(index.frames[i], whole.frames[i]))
          << "prefix " << n << " frame " << i;
    }
    ExpectSpansInBounds(index, n);
    last_count = index.frames.size();
  }
  EXPECT_EQ(last_count, whole.frames.size());
}

TEST(WebmCorpusTest, EveryPrefixIsTruncated) {
  CheckEveryPrefix("vp9_single_frame.webm");
  CheckEveryPrefix("vp9_with_audio.webm");
}

// Reads the size vint at pos; returns its length and the value.
int ReadSize(const Bytes& b, size_t pos, uint64_t* value) {
  int len = 1;
  while (!(b[pos] & (0x80 >> (len - 1)))) ++len;
  uint64_t v = b[pos] & (0xFFu >> len);
  for (int i = 1; i < len; ++i) v = (v << 8) | b[pos + static_cast<size_t>(i)];
  *value = v;
  return len;
}

int IdLength(uint8_t first) {
  int len = 1;
  while (!(first & (0x80 >> (len - 1)))) ++len;
  return len;
}

// Rewrites the size of `pos`'s element to "unknown" of the same length.
void MakeUnknown(Bytes* b, size_t size_pos, int len) {
  (*b)[size_pos] = static_cast<uint8_t>(0xFF >> (len - 1));
  for (int i = 1; i < len; ++i) (*b)[size_pos + static_cast<size_t>(i)] = 0xFF;
}

// The same file as written by a live muxer: unknown-size Segment and
// Clusters (spike A, pitfall 12).
Bytes LiveVariant(const Bytes& in) {
  Bytes out = in;
  uint64_t size = 0;
  size_t pos = static_cast<size_t>(IdLength(in[0]));  // EBML header.
  pos += static_cast<size_t>(ReadSize(in, pos, &size)) + size;
  pos += static_cast<size_t>(IdLength(in[pos]));  // Segment ID.
  const int seg_len = ReadSize(in, pos, &size);
  MakeUnknown(&out, pos, seg_len);
  pos += static_cast<size_t>(seg_len);
  const size_t seg_end = pos + size;
  while (pos < seg_end) {
    const int id_len = IdLength(in[pos]);
    uint32_t id = 0;
    for (int i = 0; i < id_len; ++i) id = (id << 8) | in[pos + i];
    pos += static_cast<size_t>(id_len);
    const int len = ReadSize(in, pos, &size);
    if (id == kCluster) MakeUnknown(&out, pos, len);
    pos += static_cast<size_t>(len) + size;
  }
  return out;
}

TEST(WebmCorpusTest, UnknownSizesIndexTheSame) {
  for (const wt_test::Json& row : wt_test::Manifest()["files"].array) {
    if (row["kind"].string != "webm" || row["expect"].string != "ok") continue;
    const std::string name = row["name"].string;
    SCOPED_TRACE(name);
    const Bytes original = ReadAll(kAssets / name);
    const Bytes live = LiveVariant(original);
    ASSERT_NE(live, original);
    wt::WebmIndex a, b;
    ASSERT_EQ(Demux(original, &a), WT_OK);
    ASSERT_EQ(Demux(live, &b), WT_OK) << b.error;
    ASSERT_EQ(a.frames.size(), b.frames.size());
    for (size_t i = 0; i < a.frames.size(); ++i) {
      EXPECT_TRUE(SameFrame(a.frames[i], b.frames[i])) << "frame " << i;
      EXPECT_EQ(a.frames[i].duration_ns, b.frames[i].duration_ns);
    }
  }
}

TEST(WebmCorpusTest, FrameLimit) {
  const Bytes bytes = ReadAll(kAssets / "vp9_alpha_512.webm");
  wt::WebmIndex index;
  wt_limits limits = DefaultLimits();
  limits.max_frames = 89;
  EXPECT_EQ(Demux(bytes, &index, limits), WT_ERR_LIMIT);
  EXPECT_TRUE(index.frames.empty());
  limits.max_frames = 90;
  EXPECT_EQ(Demux(bytes, &index, limits), WT_OK);
  EXPECT_EQ(index.frames.size(), 90u);
}

// Colour and alpha come from independent encoders; a frame is a random-access
// point only when both payloads are keyframes (spike A, pitfall 1).
TEST(WebmCorpusTest, RandomAccessNeedsColourAndAlphaKeyframes) {
  const Bytes src = ReadAll(kAssets / "vp9_alpha_512.webm");
  wt::WebmIndex index;
  ASSERT_EQ(Demux(src, &index), WT_OK);
  auto payload = [&](wt::Span s) {
    return Bytes(src.begin() + static_cast<long>(s.offset),
                 src.begin() + static_cast<long>(s.offset + s.size));
  };
  const Bytes key_colour = payload(index.frames[0].data);
  const Bytes key_alpha = payload(index.frames[0].alpha);
  const Bytes inter_colour = payload(index.frames[1].data);
  const Bytes inter_alpha = payload(index.frames[1].alpha);

  // Both container-flagged keyframes (no ReferenceBlock).
  const Bytes file = File(
      VideoTrack(1, "V_VP9", 512, 512),
      Cluster(0,
              Cat({BlockGroup(1, 0, AlphaAdditions(key_alpha), key_colour),
                   BlockGroup(1, 33, AlphaAdditions(inter_alpha), key_colour),
                   BlockGroup(1, 67, AlphaAdditions(key_alpha), inter_colour),
                   BlockGroup(1, 100, {}, key_colour)})));
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  ASSERT_EQ(index.frames.size(), 4u);
  for (const wt::WebmFrame& f : index.frames) EXPECT_TRUE(f.keyframe);
  EXPECT_TRUE(index.frames[0].random_access);
  EXPECT_FALSE(index.frames[1].random_access);  // Alpha is an inter frame.
  EXPECT_FALSE(index.frames[2].random_access);  // Colour is an inter frame.
  EXPECT_TRUE(index.frames[3].random_access);   // No alpha payload.
}

// --- Hand-built files ------------------------------------------------------

TEST(WebmDemuxTest, MinimalFile) {
  const Bytes file = OneFrameFile();
  wt::WebmIndex index;
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  EXPECT_STREQ(index.error, "");
  EXPECT_TRUE(index.has_video);
  EXPECT_FALSE(index.has_audio);
  EXPECT_EQ(index.video.number, 1u);
  EXPECT_EQ(index.video.codec, WT_CODEC_VP9);
  EXPECT_EQ(index.video.width, 64u);
  EXPECT_EQ(index.video.height, 48u);
  ASSERT_EQ(index.frames.size(), 1u);
  const wt::WebmFrame& f = index.frames[0];
  EXPECT_EQ(f.data.size, kPayload.size());
  EXPECT_TRUE(std::equal(kPayload.begin(), kPayload.end(),
                         file.begin() + static_cast<long>(f.data.offset)));
  EXPECT_TRUE(f.alpha.empty());
  EXPECT_TRUE(f.keyframe);
  EXPECT_FALSE(f.random_access);  // Not a real VP9 frame.
  EXPECT_EQ(f.duration_ns, 1000000000 / 30);
}

TEST(WebmDemuxTest, Arguments) {
  wt::WebmIndex index;
  wt_limits limits = DefaultLimits();
  limits.max_frames = 0;
  EXPECT_EQ(Demux(OneFrameFile(), &index, limits), WT_ERR_INVALID_ARGUMENT);
  limits = DefaultLimits();
  limits.max_dimension = 0;
  EXPECT_EQ(Demux(OneFrameFile(), &index, limits), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::DemuxWebm(wt::ByteView{nullptr, 1}, DefaultLimits(), &index),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::DemuxWebm(wt::ByteView{}, DefaultLimits(), &index),
            WT_ERR_TRUNCATED);
  EXPECT_TRUE(index.frames.empty());
}

TEST(WebmDemuxTest, ResetsOutput) {
  wt::WebmIndex index;
  ASSERT_EQ(Demux(OneFrameFile(), &index), WT_OK);
  const Bytes audio_only = File(AudioTrack(1), Cluster(0, SimpleBlock(1, 0)));
  EXPECT_EQ(Demux(audio_only, &index), WT_ERR_UNSUPPORTED);
  EXPECT_FALSE(index.has_video);
  EXPECT_TRUE(index.has_audio);
  EXPECT_TRUE(index.frames.empty());
}

TEST(WebmDemuxTest, HeaderChecks) {
  wt::WebmIndex index;
  const Bytes body =
      El(kSegment,
         Cat({El(kTracks, VideoTrack(1)), Cluster(0, SimpleBlock(1, 0))}));
  EXPECT_EQ(Demux(Cat({EbmlHeader("matroska"), body}), &index), WT_OK);
  EXPECT_EQ(Demux(Cat({EbmlHeader("mkv"), body}), &index), WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(Cat({El(kEbml, {}), body}), &index), WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(body, &index), WT_ERR_INVALID_DATA);       // No EBML header.
  EXPECT_EQ(Demux(EbmlHeader(), &index), WT_ERR_TRUNCATED);  // No Segment.
  // Short inputs: a prefix of the EBML magic is truncated, anything else is
  // invalid.
  EXPECT_EQ(Demux(Bytes{0x1A, 0x45}, &index), WT_ERR_TRUNCATED);
  EXPECT_EQ(Demux(Bytes{0x1A, 0x46}, &index), WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(Bytes{'R'}, &index), WT_ERR_INVALID_DATA);
  // DocType may be NUL padded.
  EXPECT_EQ(
      Demux(Cat({El(kEbml, S(kDocType, std::string("webm\0\0", 6))), body}),
            &index),
      WT_OK);
}

TEST(WebmDemuxTest, TrackSelection) {
  wt::WebmIndex index;
  // An audio track before the video; audio blocks are skipped.
  Bytes file = File(Cat({AudioTrack(1), VideoTrack(2)}),
                    Cluster(0, Cat({SimpleBlock(1, 0), SimpleBlock(2, 0),
                                    SimpleBlock(1, 10), SimpleBlock(2, 33)})));
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  EXPECT_TRUE(index.has_audio);
  EXPECT_EQ(index.video.number, 2u);
  EXPECT_EQ(index.frames.size(), 2u);

  // The first VP8/VP9 track wins; an unknown codec is skipped over.
  file = File(Cat({VideoTrack(1, "V_AV1"), VideoTrack(2, "V_VP8"),
                   VideoTrack(3, "V_VP9")}),
              Cluster(0, Cat({SimpleBlock(1, 0), SimpleBlock(2, 0),
                              SimpleBlock(3, 0)})));
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  EXPECT_EQ(index.video.number, 2u);
  EXPECT_EQ(index.video.codec, WT_CODEC_VP8);
  EXPECT_EQ(index.frames.size(), 1u);

  // No VP8/VP9 track at all.
  file = File(VideoTrack(1, "V_AV1"), Cluster(0, SimpleBlock(1, 0)));
  EXPECT_EQ(Demux(file, &index), WT_ERR_UNSUPPORTED);
}

TEST(WebmDemuxTest, TrackChecks) {
  wt::WebmIndex index;
  const Bytes cluster = Cluster(0, SimpleBlock(1, 0));
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", 0, 48), cluster), &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(VideoTrack(0), cluster), &index), WT_ERR_INVALID_DATA);
  EXPECT_EQ(
      Demux(File(VideoTrack(1, "V_VP9", 64, 48, 1, El(kContentEncodings, {})),
                 cluster),
            &index),
      WT_ERR_UNSUPPORTED);
  // ContentEncodings on another track don't matter.
  EXPECT_EQ(
      Demux(File(Cat({El(kTrackEntry, Cat({U(kTrackNumber, 2), U(kTrackType, 2),
                                           El(kContentEncodings, {})})),
                      VideoTrack(1)}),
                 cluster),
            &index),
      WT_OK);
  // A video track without blocks.
  EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(0, {})), &index),
            WT_ERR_INVALID_DATA);
}

TEST(WebmDemuxTest, DimensionLimit) {
  wt::WebmIndex index;
  const Bytes cluster = Cluster(0, SimpleBlock(1, 0));
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", 1024, 1024), cluster), &index),
            WT_OK);
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", 1025, 16), cluster), &index),
            WT_ERR_LIMIT);
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", 16, 1025), cluster), &index),
            WT_ERR_LIMIT);
  // Values wider than 32 bits must not wrap.
  EXPECT_EQ(
      Demux(File(VideoTrack(1, "V_VP9", (uint64_t{1} << 32) + 16, 16), cluster),
            &index),
      WT_ERR_LIMIT);
}

// T6.1 (S-03): a caller may raise max_dimension, but never past what a frame
// can be decoded at (YuvaImage::kMaxDimension), as WebP and GIF already
// enforce. Before the fix a 2^31 x 2^31 track (2^62 px a frame) under a
// budget of 3 x 2^62 px indexed 5 frames: the 4th frame's (4 x 2^62)
// product in Push() wrapped to 0.
TEST(WebmDemuxTest, RaisedDimensionLimitStopsAtTheDecodableSize) {
  wt::WebmIndex index;
  const Bytes cluster = Cluster(0, SimpleBlock(1, 0));
  wt_limits wide = DefaultLimits();
  wide.max_dimension = UINT32_MAX;
  const uint32_t max = wt::YuvaImage::kMaxDimension;
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", max, 16), cluster), &index, wide),
            WT_OK);
  EXPECT_EQ(
      Demux(File(VideoTrack(1, "V_VP9", max + 1, 16), cluster), &index, wide),
      WT_ERR_LIMIT);
  EXPECT_EQ(
      Demux(File(VideoTrack(1, "V_VP9", 16, max + 1), cluster), &index, wide),
      WT_ERR_LIMIT);

  const uint64_t side = uint64_t{1} << 31;
  wide.max_total_pixels = 3 * (uint64_t{1} << 62);
  Bytes five;
  for (int16_t i = 0; i < 5; ++i) five = Cat({five, SimpleBlock(1, i)});
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", side, side), Cluster(0, five)),
                  &index, wide),
            WT_ERR_LIMIT);
  EXPECT_TRUE(index.frames.empty());
}

// Decode budget: width x height x frames, checked as frames are added.
TEST(WebmDemuxTest, PixelBudget) {
  wt::WebmIndex index;
  const Bytes three =
      Cat({SimpleBlock(1, 0), SimpleBlock(1, 33), SimpleBlock(1, 66)});
  const Bytes file = File(VideoTrack(1, "V_VP9", 64, 48), Cluster(0, three));
  wt_limits limits = DefaultLimits();
  limits.max_total_pixels = uint64_t{64} * 48 * 3;
  EXPECT_EQ(Demux(file, &index, limits), WT_OK);
  limits.max_total_pixels -= 1;
  EXPECT_EQ(Demux(file, &index, limits), WT_ERR_LIMIT);
  EXPECT_TRUE(index.frames.empty());
  // One frame at the full default dimension fits the default budget.
  EXPECT_EQ(Demux(File(VideoTrack(1, "V_VP9", 1024, 1024),
                       Cluster(0, SimpleBlock(1, 0))),
                  &index),
            WT_OK);
}

// Duration: the sum of every frame's display duration, checked at the end.
TEST(WebmDemuxTest, DurationLimit) {
  wt::WebmIndex index;
  // Frames at 0, 40 and 100 ms; the last takes its BlockDuration (25 ms).
  const Bytes three = Cat({SimpleBlock(1, 0), SimpleBlock(1, 40),
                           BlockGroup(1, 100, U(kBlockDuration, 25))});
  const Bytes file = File(VideoTrack(1), Cluster(0, three));
  wt_limits limits = DefaultLimits();
  limits.max_duration_ms = 125;
  EXPECT_EQ(Demux(file, &index, limits), WT_OK);
  limits.max_duration_ms = 124;
  EXPECT_EQ(Demux(file, &index, limits), WT_ERR_LIMIT);
  EXPECT_TRUE(index.frames.empty());
  EXPECT_STREQ(index.error, "duration over max_duration_ms");
}

TEST(WebmDemuxTest, ZeroLimitsAreInvalidArguments) {
  wt::WebmIndex index;
  const Bytes file = OneFrameFile();
  wt_limits limits = DefaultLimits();
  limits.max_total_pixels = 0;
  EXPECT_EQ(Demux(file, &index, limits), WT_ERR_INVALID_ARGUMENT);
  limits = DefaultLimits();
  limits.max_duration_ms = 0;
  EXPECT_EQ(Demux(file, &index, limits), WT_ERR_INVALID_ARGUMENT);
}

TEST(WebmDemuxTest, LacedVideoIsUnsupported) {
  wt::WebmIndex index;
  for (uint8_t lacing : {0x02, 0x04, 0x06}) {
    const Bytes block = El(kSimpleBlock, BlockBody(1, 0, 0x80 | lacing));
    EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(0, block)), &index),
              WT_ERR_UNSUPPORTED);
  }
  // Laced audio is fine: it is never looked at.
  const Bytes audio = El(kSimpleBlock, BlockBody(2, 0, 0x86));
  EXPECT_EQ(Demux(File(Cat({VideoTrack(1), AudioTrack(2)}),
                       Cluster(0, Cat({audio, SimpleBlock(1, 0)}))),
                  &index),
            WT_OK);
}

// A VP8 keyframe header claiming width x height (see decode_test.cc).
Bytes Vp8Keyframe(uint32_t width, uint32_t height) {
  const uint32_t tag = 64u << 5 | 1u << 4;  // Keyframe, shown, 64-byte part.
  Bytes b = {static_cast<uint8_t>(tag),
             static_cast<uint8_t>(tag >> 8),
             static_cast<uint8_t>(tag >> 16),
             0x9d,
             0x01,
             0x2a,
             static_cast<uint8_t>(width),
             static_cast<uint8_t>((width >> 8) & 0x3f),
             static_cast<uint8_t>(height),
             static_cast<uint8_t>((height >> 8) & 0x3f)};
  b.resize(b.size() + 128, 0);
  return b;
}

// T6.1 (S-01): a keyframe sets the size libvpx allocates for, and VP8 has no
// --size-limit, so a colour or alpha keyframe that disagrees with the
// track's PixelWidth x PixelHeight (which max_dimension bounds) makes the
// file invalid. Before the fix a 16 x 16 track with a 16383 x 16383 VP8
// keyframe probed OK.
TEST(WebmDemuxTest, KeyframeSizeMustMatchTheTrack) {
  wt::WebmIndex index;
  const Bytes right = Vp8Keyframe(16, 16);
  const Bytes huge = Vp8Keyframe(16383, 16383);
  const Bytes track = VideoTrack(1, "V_VP8", 16, 16, 1);
  EXPECT_EQ(
      Demux(File(track, Cluster(0, SimpleBlock(1, 0, true, right))), &index),
      WT_OK);
  EXPECT_TRUE(index.frames[0].random_access);
  EXPECT_EQ(
      Demux(File(track, Cluster(0, SimpleBlock(1, 0, true, huge))), &index),
      WT_ERR_INVALID_DATA);
  EXPECT_STREQ(index.error, "keyframe size differs from the track");
  EXPECT_TRUE(index.frames.empty());
  // The same for the alpha stream's keyframes.
  EXPECT_EQ(Demux(File(track, Cluster(0, BlockGroup(1, 0, AlphaAdditions(huge),
                                                    right))),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(track, Cluster(0, BlockGroup(1, 0, AlphaAdditions(right),
                                                    right))),
                  &index),
            WT_OK);
  // Only one byte off.
  EXPECT_EQ(
      Demux(
          File(track, Cluster(0, SimpleBlock(1, 0, true, Vp8Keyframe(16, 17)))),
          &index),
      WT_ERR_INVALID_DATA);
}

TEST(WebmDemuxTest, Alpha) {
  wt::WebmIndex index;
  const Bytes alpha = {0xAA, 0xBB};
  // BlockAddID defaults to 1; explicit 1 also works; 2 is something else.
  const Bytes clusters = Cluster(
      0, Cat({BlockGroup(1, 0, AlphaAdditions(alpha)),
              BlockGroup(1, 33, AlphaAdditions(alpha, 1)),
              BlockGroup(1, 67, AlphaAdditions(alpha, 2)),
              BlockGroup(1, 100, El(kBlockAdditions, {})), SimpleBlock(1, 133),
              BlockGroup(1, 167, AlphaAdditions({}))}));
  ASSERT_EQ(Demux(File(VideoTrack(1), clusters), &index), WT_OK) << index.error;
  ASSERT_EQ(index.frames.size(), 6u);
  const std::vector<bool> has = {true, true, false, false, false, false};
  for (size_t i = 0; i < has.size(); ++i) {
    EXPECT_EQ(!index.frames[i].alpha.empty(), has[i]) << "frame " << i;
  }
  EXPECT_EQ(index.frames[0].alpha.size, alpha.size());

  // AlphaMode other than 1: BlockAdditions are ignored.
  for (uint64_t mode : {0, 2}) {
    ASSERT_EQ(
        Demux(File(VideoTrack(1, "V_VP9", 64, 48, mode), clusters), &index),
        WT_OK);
    EXPECT_FALSE(index.video.has_alpha());
    for (const wt::WebmFrame& f : index.frames) EXPECT_TRUE(f.alpha.empty());
  }

  // Too many BlockMores.
  Bytes mores;
  for (int i = 0; i < 9; ++i) mores = Cat({mores, El(kBlockMore, {})});
  EXPECT_EQ(
      Demux(File(VideoTrack(1),
                 Cluster(0, BlockGroup(1, 0, El(kBlockAdditions, mores)))),
            &index),
      WT_ERR_LIMIT);
}

TEST(WebmDemuxTest, KeyframeFlags) {
  wt::WebmIndex index;
  const Bytes clusters = Cluster(
      0, Cat({SimpleBlock(1, 0, true), SimpleBlock(1, 33, false),
              BlockGroup(1, 67), BlockGroup(1, 100, U(kReferenceBlock, 1))}));
  ASSERT_EQ(Demux(File(VideoTrack(1), clusters), &index), WT_OK);
  ASSERT_EQ(index.frames.size(), 4u);
  EXPECT_TRUE(index.frames[0].keyframe);
  EXPECT_FALSE(index.frames[1].keyframe);
  EXPECT_TRUE(index.frames[2].keyframe);
  EXPECT_FALSE(index.frames[3].keyframe);
}

TEST(WebmDemuxTest, BlockGroupChecks) {
  wt::WebmIndex index;
  const Bytes two_blocks =
      El(kBlockGroup,
         Cat({El(kBlock, BlockBody(1, 0, 0)), El(kBlock, BlockBody(1, 0, 0))}));
  EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(0, two_blocks)), &index),
            WT_ERR_INVALID_DATA);
  // A BlockGroup without a Block is skipped.
  EXPECT_EQ(Demux(File(VideoTrack(1),
                       Cluster(0, Cat({El(kBlockGroup, U(kBlockDuration, 5)),
                                       SimpleBlock(1, 0)}))),
                  &index),
            WT_OK);
  EXPECT_EQ(index.frames.size(), 1u);
  // Blocks with no frame bytes, or a header cut short.
  EXPECT_EQ(Demux(File(VideoTrack(1),
                       Cluster(0, El(kSimpleBlock, BlockBody(1, 0, 0x80, {})))),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(0, El(kSimpleBlock, {0x81, 0}))),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(
      Demux(File(VideoTrack(1), Cluster(0, El(kSimpleBlock, {}))), &index),
      WT_ERR_INVALID_DATA);
}

TEST(WebmDemuxTest, Timestamps) {
  wt::WebmIndex index;
  // TimecodeScale 500 us; Cluster Timecode plus signed block offsets.
  const Bytes info = El(kInfo, U(kTimecodeScale, 500000));
  const Bytes file =
      File(VideoTrack(1),
           Cat({Cluster(100, Cat({SimpleBlock(1, -100), SimpleBlock(1, 60)})),
                Cluster(200, SimpleBlock(1, 20))}),
           info);
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  EXPECT_EQ(index.timecode_scale_ns, 500000u);
  ASSERT_EQ(index.frames.size(), 3u);
  EXPECT_EQ(index.frames[0].pts_ns, 0);
  EXPECT_EQ(index.frames[1].pts_ns, 80000000);
  EXPECT_EQ(index.frames[2].pts_ns, 110000000);
  EXPECT_EQ(index.frames[0].duration_ns, 80000000);
  EXPECT_EQ(index.frames[1].duration_ns, 30000000);

  // A negative pts is allowed.
  ASSERT_EQ(Demux(File(VideoTrack(1), Cluster(0, SimpleBlock(1, -5))), &index),
            WT_OK);
  EXPECT_EQ(index.frames[0].pts_ns, -5000000);

  // Overflow is invalid, not wrapped.
  EXPECT_EQ(
      Demux(File(VideoTrack(1), Cluster(uint64_t{1} << 62, SimpleBlock(1, 0))),
            &index),
      WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(VideoTrack(1),
                       Cluster((uint64_t{1} << 62) + 1, SimpleBlock(1, 0))),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(1, SimpleBlock(1, 0)),
                       El(kInfo, U(kTimecodeScale, uint64_t{1} << 63))),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(File(VideoTrack(1), Cluster(0, SimpleBlock(1, 0)),
                       El(kInfo, U(kTimecodeScale, 0))),
                  &index),
            WT_ERR_INVALID_DATA);
  // A block before its Cluster's Timecode.
  EXPECT_EQ(Demux(File(VideoTrack(1),
                       El(kCluster, Cat({SimpleBlock(1, 0), U(kTimecode, 0)}))),
                  &index),
            WT_ERR_INVALID_DATA);
}

TEST(WebmDemuxTest, InfoAndTracksAfterAClusterAreIgnored) {
  wt::WebmIndex index;
  const Bytes file = Cat(
      {EbmlHeader(),
       El(kSegment,
          Cat({El(kTracks, VideoTrack(1)), Cluster(0, SimpleBlock(1, 0)),
               El(kInfo, U(kTimecodeScale, 1)),
               El(kTracks, VideoTrack(2, "V_VP8", 8, 8)),
               Cluster(33, Cat({SimpleBlock(1, 0), SimpleBlock(2, 0)}))}))});
  ASSERT_EQ(Demux(file, &index), WT_OK) << index.error;
  EXPECT_EQ(index.timecode_scale_ns, 1000000u);
  EXPECT_EQ(index.video.number, 1u);
  ASSERT_EQ(index.frames.size(), 2u);
  EXPECT_EQ(index.frames[1].pts_ns, 33000000);
}

TEST(WebmDemuxTest, Durations) {
  wt::WebmIndex index;
  const Bytes three = Cat({SimpleBlock(1, 0), SimpleBlock(1, 40),
                           BlockGroup(1, 100, U(kBlockDuration, 25))});
  // Next-pts differences; the last frame takes its BlockDuration.
  ASSERT_EQ(Demux(File(VideoTrack(1), Cluster(0, three)), &index), WT_OK);
  ASSERT_EQ(index.frames.size(), 3u);
  EXPECT_EQ(index.frames[0].duration_ns, 40000000);
  EXPECT_EQ(index.frames[1].duration_ns, 60000000);
  EXPECT_EQ(index.frames[2].duration_ns, 25000000);

  // Without BlockDuration: DefaultDuration, else 1/30 s.
  const Bytes two = Cat({SimpleBlock(1, 0), SimpleBlock(1, 50)});
  ASSERT_EQ(Demux(File(VideoTrack(1, "V_VP9", 64, 48, 1,
                                  U(kDefaultDuration, 20000000)),
                       Cluster(0, two)),
                  &index),
            WT_OK);
  EXPECT_EQ(index.video.default_duration_ns, 20000000);
  EXPECT_EQ(index.frames[0].duration_ns, 50000000);
  EXPECT_EQ(index.frames[1].duration_ns, 20000000);
  ASSERT_EQ(Demux(File(VideoTrack(1), Cluster(0, two)), &index), WT_OK);
  EXPECT_EQ(index.frames[1].duration_ns, 33333333);

  // Equal or decreasing timestamps fall back too.
  const Bytes odd =
      Cat({SimpleBlock(1, 50), SimpleBlock(1, 50),
           BlockGroup(1, 10, U(kBlockDuration, 7)), SimpleBlock(1, 30)});
  ASSERT_EQ(Demux(File(VideoTrack(1), Cluster(0, odd)), &index), WT_OK);
  EXPECT_EQ(index.frames[0].duration_ns, 33333333);
  EXPECT_EQ(index.frames[1].duration_ns, 33333333);
  EXPECT_EQ(index.frames[2].duration_ns, 20000000);
  EXPECT_EQ(index.frames[3].duration_ns, 33333333);

  // A BlockDuration or DefaultDuration too big for int64 is ignored.
  ASSERT_EQ(
      Demux(File(VideoTrack(1, "V_VP9", 64, 48, 1,
                            U(kDefaultDuration, uint64_t{1} << 63)),
                 Cluster(0, BlockGroup(1, 0,
                                       U(kBlockDuration, uint64_t{1} << 62)))),
            &index),
      WT_OK);
  EXPECT_EQ(index.video.default_duration_ns, 0);
  EXPECT_EQ(index.frames[0].duration_ns, 33333333);
}

TEST(WebmDemuxTest, StructuralErrors) {
  wt::WebmIndex index;
  const Bytes tracks = El(kTracks, VideoTrack(1));
  const Bytes cluster = Cluster(0, SimpleBlock(1, 0));

  // A child that overflows its known-size parent.
  Bytes bad = Cat({EbmlHeader(), El(kSegment, Cat({tracks, cluster}))});
  bad.push_back(0);                      // One byte past the Segment...
  EXPECT_EQ(Demux(bad, &index), WT_OK);  // ...after it is ignored.
  Bytes inner = Cluster(0, SimpleBlock(1, 0));
  // Grow the SimpleBlock's size by one inside an unchanged Cluster.
  const size_t sb = inner.size() - (kPayload.size() + 4) - 8;
  ASSERT_EQ(inner[sb - 1], static_cast<uint8_t>(kSimpleBlock));
  inner[sb + 7] = static_cast<uint8_t>(inner[sb + 7] + 1);
  EXPECT_EQ(
      Demux(Cat({EbmlHeader(), El(kSegment, Cat({tracks, inner}))}), &index),
      WT_ERR_INVALID_DATA);

  // Unknown sizes are only for Segment and Cluster.
  EXPECT_EQ(
      Demux(Cat({EbmlHeader(),
                 UnknownEl(kSegment,
                           Cat({UnknownEl(kTracks, VideoTrack(1)), cluster}))}),
            &index),
      WT_ERR_INVALID_DATA);
  EXPECT_EQ(
      Demux(
          Cat({EbmlHeader(),
               UnknownEl(
                   kSegment,
                   Cat({tracks,
                        UnknownEl(kCluster,
                                  Cat({U(kTimecode, 0),
                                       UnknownEl(kSimpleBlock,
                                                 BlockBody(1, 0, 0x80))}))}))}),
          &index),
      WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(Cat({UnknownEl(kEbml, S(kDocType, "webm")),
                       El(kSegment, Cat({tracks, cluster}))}),
                  &index),
            WT_ERR_INVALID_DATA);

  // An unknown-size Cluster ends at the next non-child (here Cues, then a
  // second Cluster).
  const Bytes live = Cat(
      {EbmlHeader(),
       UnknownEl(
           kSegment,
           Cat({tracks,
                UnknownEl(kCluster, Cat({U(kTimecode, 0), SimpleBlock(1, 0)})),
                El(kCues, {1, 2, 3}),
                UnknownEl(kCluster,
                          Cat({U(kTimecode, 33), SimpleBlock(1, 0)}))}))});
  ASSERT_EQ(Demux(live, &index), WT_OK) << index.error;
  EXPECT_EQ(index.frames.size(), 2u);

  // Bad vints: an ID longer than 4 bytes, a size longer than 8.
  EXPECT_EQ(Demux(Cat({EbmlHeader(), El(kSegment, Bytes{0x08, 0x00, 0x00, 0x00,
                                                        0x00, 0x80})}),
                  &index),
            WT_ERR_INVALID_DATA);
  EXPECT_EQ(Demux(Bytes{0x1A, 0x45, 0xDF, 0xA3, 0x00, 0x00}, &index),
            WT_ERR_INVALID_DATA);
  // A Segment with no Tracks, or only an unknown-size top-level element.
  EXPECT_EQ(Demux(Cat({EbmlHeader(), El(kSegment, cluster)}), &index),
            WT_ERR_UNSUPPORTED);
  EXPECT_EQ(Demux(Cat({EbmlHeader(), UnknownEl(kCluster, {})}), &index),
            WT_ERR_INVALID_DATA);
}

TEST(WebmDemuxTest, TruncationKeepsCompleteFrames) {
  const Bytes clusters =
      Cat({Cluster(0, Cat({SimpleBlock(1, 0), BlockGroup(1, 33)})),
           Cluster(67, SimpleBlock(1, 0))});
  const Bytes file = File(VideoTrack(1), clusters);
  wt::WebmIndex whole;
  ASSERT_EQ(Demux(file, &whole), WT_OK);
  ASSERT_EQ(whole.frames.size(), 3u);
  size_t last = 0;
  for (size_t n = 0; n < file.size(); ++n) {
    wt::WebmIndex index;
    ASSERT_EQ(
        Demux(Bytes(file.begin(), file.begin() + static_cast<long>(n)), &index),
        WT_ERR_TRUNCATED)
        << "prefix " << n;
    ASSERT_GE(index.frames.size(), last);
    last = index.frames.size();
    // A frame is only committed once its whole block is in bounds.
    for (const wt::WebmFrame& f : index.frames) {
      EXPECT_LE(f.data.offset + f.data.size, n);
    }
  }
  EXPECT_EQ(last, 2u);  // The last SimpleBlock ends at EOF.
}

}  // namespace
