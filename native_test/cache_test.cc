// Tests for the frame cache (T3.1): CRC-32, the .wsc writer and reader on
// synthetic frames (round trip, damaged and partial files, mismatches), and
// the cache inside wt::Anim and wt_anim_create_cached on the corpus (write on
// the first pass, read afterwards, delete and re-decode a bad file).
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "anim_util.h"
#include "api/anim.h"
#include "cache/crc32.h"
#include "cache/store.h"
#include "cache/wsc.h"
#include "convert/yuva.h"
#include "demux/sniff.h"
#include "demux/source.h"
#include "gtest/gtest.h"
#include "manifest.h"
#include "wt_api.h"
#include "zlib.h"

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<uint8_t>;
using wt::Anim;
using wt::ByteSource;
using wt::ByteView;
using wt::WscParams;
using wt::WscReader;
using wt::WscWriter;
using wt::YuvaImage;
using Mode = wt::Anim::CacheMode;

const fs::path kAssets = WT_TEST_ASSETS_DIR;

// A fresh directory per test, removed afterwards.
class TempDir {
 public:
  TempDir() {
    const testing::TestInfo* t =
        testing::UnitTest::GetInstance()->current_test_info();
    path_ = fs::temp_directory_path() /
            ("wt_cache_test_" + std::to_string(getpid()) + "_" +
             t->test_suite_name() + "_" + t->name());
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }
  std::string File(const std::string& name) const {
    return (path_ / name).string();
  }
  // Names of everything in the directory, sorted.
  std::vector<std::string> List() const {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(path_)) {
      names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
  }

 private:
  fs::path path_;
};

Bytes ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), {});
}

void WriteFile(const std::string& path, const Bytes& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

std::unique_ptr<ByteSource> Memory(const Bytes& bytes) {
  std::unique_ptr<ByteSource> source;
  EXPECT_EQ(
      ByteSource::CreateMemory(bytes.data(), bytes.size(), UINT64_MAX, &source),
      WT_OK);
  return source;
}

wt_limits DefaultLimits() {
  wt_limits limits;
  wt_limits_init_default(&limits);
  return limits;
}

// --- CRC-32 -----------------------------------------------------------------

TEST(Crc32Test, MatchesZlib) {
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(wt::Crc32(0, check, sizeof(check)), 0xCBF43926u);
  EXPECT_EQ(wt::Crc32(0, nullptr, 0), 0u);

  std::mt19937 rng(7);
  Bytes data(4099);
  for (uint8_t& b : data) b = static_cast<uint8_t>(rng());
  for (size_t size :
       {size_t{1}, size_t{7}, size_t{8}, size_t{9}, size_t{63}, size_t{4099}}) {
    const uint32_t expected =
        static_cast<uint32_t>(crc32(0, data.data(), static_cast<uInt>(size)));
    EXPECT_EQ(wt::Crc32(0, data.data(), size), expected) << size;
    // Split anywhere, the running CRC is the same.
    const size_t half = size / 3;
    EXPECT_EQ(wt::Crc32(wt::Crc32(0, data.data(), half), data.data() + half,
                        size - half),
              expected)
        << size;
  }
}

// --- Writer and reader on synthetic frames --------------------------------

struct Synthetic {
  WscParams params;
  wt::YuvColor color;
  std::vector<std::unique_ptr<YuvaImage>> frames;
  std::vector<uint32_t> durations;
  std::vector<int64_t> pts;
};

// Frames of random noise in a few runs (so LZ4 has something to find), each
// in its own YuvaImage.
Synthetic MakeSynthetic(uint32_t w, uint32_t h, bool alpha, uint32_t count,
                        uint32_t seed = 1) {
  Synthetic s;
  s.params.width = w;
  s.params.height = h;
  s.params.has_alpha = alpha;
  s.params.frame_count = count;
  s.params.source_size = 12345;
  s.params.source_crc = 0xDEADBEEF;
  s.color = {wt::YuvMatrix::kBt709, true};
  std::mt19937 rng(seed);
  int64_t pts = 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto img = std::make_unique<YuvaImage>();
    EXPECT_EQ(img->Allocate(w, h, alpha), WT_OK);
    uint8_t* p = img->data();
    for (size_t j = 0; j < img->data_size();) {
      const uint8_t v = static_cast<uint8_t>(rng());
      size_t run = 1 + rng() % 16;
      for (; run > 0 && j < img->data_size(); --run) p[j++] = v;
    }
    img->set_color(s.color);
    s.frames.push_back(std::move(img));
    const uint32_t d = 10 + i % 7;
    s.durations.push_back(d);
    s.pts.push_back(pts);
    pts += d + 1;  // Not the sum of durations: pts are stored, not derived.
  }
  return s;
}

wt_status WriteSynthetic(const Synthetic& s, const std::string& path) {
  std::unique_ptr<WscWriter> writer;
  wt_status st =
      WscWriter::Create(path.c_str(), s.params, s.color, UINT64_MAX, &writer);
  if (st != WT_OK) return st;
  for (size_t i = 0; i < s.frames.size(); ++i) {
    st = writer->Append(s.frames[i]->view(), s.durations[i], s.pts[i]);
    if (st != WT_OK) return st;
  }
  return writer->Finish();
}

wt_status OpenReader(const Bytes& file, const WscParams& expect,
                     std::unique_ptr<WscReader>* reader,
                     const wt_limits& limits = DefaultLimits()) {
  return WscReader::Open(Memory(file), expect, limits, reader);
}

// Reads every frame and compares it with the original.
void ExpectFramesMatch(WscReader* reader, const Synthetic& s) {
  YuvaImage out;
  ASSERT_EQ(out.Allocate(s.params.width, s.params.height, s.params.has_alpha),
            WT_OK);
  for (uint32_t i = 0; i < s.params.frame_count; ++i) {
    ASSERT_EQ(reader->ReadFrame(i, &out), WT_OK) << i;
    ASSERT_EQ(std::memcmp(out.data(), s.frames[i]->data(), out.data_size()), 0)
        << "frame " << i;
    EXPECT_EQ(out.color().matrix, s.color.matrix);
    EXPECT_EQ(out.color().full_range, s.color.full_range);
    EXPECT_EQ(reader->frame(i).duration_ms, s.durations[i]);
    EXPECT_EQ(reader->frame(i).pts_ms, s.pts[i]);
  }
}

TEST(WscTest, RoundTrip) {
  TempDir dir;
  struct Case {
    uint32_t w, h;
    bool alpha;
    uint32_t count;
  };
  for (const Case& c : {Case{64, 64, true, 5}, Case{1, 1, false, 1},
                        Case{301, 199, true, 3}, Case{17, 33, false, 4}}) {
    SCOPED_TRACE(testing::Message() << c.w << "x" << c.h << " alpha " << c.alpha
                                    << " frames " << c.count);
    const Synthetic s = MakeSynthetic(c.w, c.h, c.alpha, c.count);
    const std::string path = dir.File("a.wsc");
    ASSERT_EQ(WriteSynthetic(s, path), WT_OK);
    // Only the final file is left.
    EXPECT_EQ(dir.List(), std::vector<std::string>{"a.wsc"});

    const Bytes file = ReadFile(path);
    EXPECT_LE(file.size(), wt::WscMaxFileSize(s.params));
    std::unique_ptr<WscReader> reader;
    ASSERT_EQ(OpenReader(file, s.params, &reader), WT_OK);
    ExpectFramesMatch(reader.get(), s);
    // Twice: the second read skips the CRC and must be identical.
    ExpectFramesMatch(reader.get(), s);

    // The same through a file mapping.
    std::unique_ptr<ByteSource> mapped;
    ASSERT_EQ(ByteSource::OpenFile(path.c_str(), wt::WscMaxFileSize(s.params),
                                   &mapped),
              WT_OK);
    ASSERT_EQ(
        WscReader::Open(std::move(mapped), s.params, DefaultLimits(), &reader),
        WT_OK);
    ExpectFramesMatch(reader.get(), s);
  }
}

TEST(WscTest, ReplacesExistingFileAtomically) {
  TempDir dir;
  const std::string path = dir.File("a.wsc");
  const Synthetic s1 = MakeSynthetic(32, 32, true, 2, 1);
  const Synthetic s2 = MakeSynthetic(32, 32, true, 2, 2);
  ASSERT_EQ(WriteSynthetic(s1, path), WT_OK);
  // A reader of the first file keeps it after the second replaces it.
  std::unique_ptr<ByteSource> mapped;
  ASSERT_EQ(ByteSource::OpenFile(path.c_str(), UINT64_MAX, &mapped), WT_OK);
  std::unique_ptr<WscReader> old_reader;
  ASSERT_EQ(WscReader::Open(std::move(mapped), s1.params, DefaultLimits(),
                            &old_reader),
            WT_OK);
  ASSERT_EQ(WriteSynthetic(s2, path), WT_OK);
  ExpectFramesMatch(old_reader.get(), s1);
  std::unique_ptr<WscReader> reader;
  ASSERT_EQ(OpenReader(ReadFile(path), s2.params, &reader), WT_OK);
  ExpectFramesMatch(reader.get(), s2);
}

TEST(WscTest, TwoWritersOneFile) {
  TempDir dir;
  const std::string path = dir.File("a.wsc");
  const Synthetic s = MakeSynthetic(48, 48, true, 3);
  std::unique_ptr<WscWriter> a, b;
  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, UINT64_MAX, &a),
            WT_OK);
  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, UINT64_MAX, &b),
            WT_OK);
  for (size_t i = 0; i < s.frames.size(); ++i) {
    ASSERT_EQ(a->Append(s.frames[i]->view(), s.durations[i], s.pts[i]), WT_OK);
    ASSERT_EQ(b->Append(s.frames[i]->view(), s.durations[i], s.pts[i]), WT_OK);
  }
  ASSERT_EQ(a->Finish(), WT_OK);
  ASSERT_EQ(b->Finish(), WT_OK);
  EXPECT_EQ(dir.List(), std::vector<std::string>{"a.wsc"});
  std::unique_ptr<WscReader> reader;
  ASSERT_EQ(OpenReader(ReadFile(path), s.params, &reader), WT_OK);
  ExpectFramesMatch(reader.get(), s);
}

TEST(WscTest, WriterErrors) {
  TempDir dir;
  const Synthetic s = MakeSynthetic(16, 16, true, 2);
  const std::string path = dir.File("a.wsc");
  std::unique_ptr<WscWriter> w;

  EXPECT_EQ(WscWriter::Create(nullptr, s.params, s.color, UINT64_MAX, &w),
            WT_ERR_INVALID_ARGUMENT);
  WscParams bad = s.params;
  bad.frame_count = 0;
  EXPECT_EQ(WscWriter::Create(path.c_str(), bad, s.color, UINT64_MAX, &w),
            WT_ERR_INVALID_ARGUMENT);
  bad = s.params;
  bad.width = YuvaImage::kMaxDimension + 1;
  EXPECT_EQ(WscWriter::Create(path.c_str(), bad, s.color, UINT64_MAX, &w),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(WscWriter::Create(dir.File("missing/a.wsc").c_str(), s.params,
                              s.color, UINT64_MAX, &w),
            WT_ERR_IO);
  EXPECT_EQ(w, nullptr);

  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, UINT64_MAX, &w),
            WT_OK);
  // Wrong size, no alpha for an alpha file, zero duration.
  YuvaImage small;
  ASSERT_EQ(small.Allocate(8, 16, true), WT_OK);
  EXPECT_EQ(w->Append(small.view(), 10, 0), WT_ERR_INVALID_ARGUMENT);
  YuvaImage opaque;
  ASSERT_EQ(opaque.Allocate(16, 16, false), WT_OK);
  EXPECT_EQ(w->Append(opaque.view(), 10, 0), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(w->Append(s.frames[0]->view(), 0, 0), WT_ERR_INVALID_ARGUMENT);
  ASSERT_EQ(w->Append(s.frames[0]->view(), 10, 0), WT_OK);
  EXPECT_EQ(w->Finish(), WT_ERR_INVALID_ARGUMENT);  // One frame short.
  ASSERT_EQ(w->Append(s.frames[1]->view(), 10, 10), WT_OK);
  EXPECT_EQ(w->Append(s.frames[1]->view(), 10, 20), WT_ERR_INVALID_ARGUMENT);
  EXPECT_FALSE(fs::exists(path));
  // Abandoning leaves nothing behind.
  w.reset();
  EXPECT_TRUE(dir.List().empty());
}

TEST(WscTest, OpaqueFileIgnoresViewAlpha) {
  TempDir dir;
  Synthetic s = MakeSynthetic(16, 8, false, 1);
  // A view with an alpha plane can feed an opaque file; alpha isn't stored.
  YuvaImage with_alpha;
  ASSERT_EQ(with_alpha.Allocate(16, 8, true), WT_OK);
  std::memcpy(with_alpha.data(), s.frames[0]->data(), s.frames[0]->data_size());
  with_alpha.set_color(s.color);
  std::unique_ptr<WscWriter> w;
  const std::string path = dir.File("a.wsc");
  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, UINT64_MAX, &w),
            WT_OK);
  ASSERT_EQ(w->Append(with_alpha.view(), s.durations[0], s.pts[0]), WT_OK);
  ASSERT_EQ(w->Finish(), WT_OK);
  std::unique_ptr<WscReader> reader;
  ASSERT_EQ(OpenReader(ReadFile(path), s.params, &reader), WT_OK);
  ExpectFramesMatch(reader.get(), s);
}

class WscCorruptionTest : public testing::Test {
 protected:
  void SetUp() override {
    s_ = MakeSynthetic(40, 24, true, 3);
    ASSERT_EQ(WriteSynthetic(s_, dir_.File("a.wsc")), WT_OK);
    file_ = ReadFile(dir_.File("a.wsc"));
    ASSERT_GT(file_.size(), wt::kWscHeaderSize + 3 * wt::kWscEntrySize);
  }

  // Rewrites the header CRC (bytes 60..63) after an intentional edit.
  static void FixHeaderCrc(Bytes* f) {
    const uint32_t crc = wt::Crc32(0, f->data(), 60);
    for (int i = 0; i < 4; ++i)
      (*f)[60 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }

  TempDir dir_;
  Synthetic s_;
  Bytes file_;
};

TEST_F(WscCorruptionTest, EveryTruncationFails) {
  for (size_t size = 0; size < file_.size(); ++size) {
    const Bytes cut(file_.begin(), file_.begin() + static_cast<long>(size));
    std::unique_ptr<WscReader> reader;
    ASSERT_EQ(OpenReader(cut, s_.params, &reader), WT_ERR_INVALID_DATA) << size;
    EXPECT_EQ(reader, nullptr);
  }
  // Trailing junk is damage too.
  Bytes longer = file_;
  longer.push_back(0);
  std::unique_ptr<WscReader> reader;
  EXPECT_EQ(OpenReader(longer, s_.params, &reader), WT_ERR_INVALID_DATA);
}

TEST_F(WscCorruptionTest, EveryHeaderOrTableBitFlipFails) {
  const size_t meta = wt::kWscHeaderSize + 3 * wt::kWscEntrySize;
  for (size_t i = 0; i < meta; ++i) {
    for (int bit : {0, 7}) {
      Bytes f = file_;
      f[i] ^= static_cast<uint8_t>(1 << bit);
      std::unique_ptr<WscReader> reader;
      const wt_status s = OpenReader(f, s_.params, &reader);
      EXPECT_TRUE(s == WT_ERR_INVALID_DATA || s == WT_ERR_UNSUPPORTED)
          << "byte " << i << " bit " << bit << ": " << wt_status_string(s);
    }
  }
}

TEST_F(WscCorruptionTest, FrameDamageFailsThatFrameOnly) {
  const size_t data_start = wt::kWscHeaderSize + 3 * wt::kWscEntrySize;
  std::unique_ptr<WscReader> probe;
  ASSERT_EQ(OpenReader(file_, s_.params, &probe), WT_OK);
  const wt::WscFrame f1 = probe->frame(1);
  ASSERT_GE(f1.offset, data_start);

  for (uint64_t at :
       {f1.offset, f1.offset + f1.size / 2, f1.offset + f1.size - 1}) {
    Bytes f = file_;
    f[at] ^= 0x10;
    std::unique_ptr<WscReader> reader;
    ASSERT_EQ(OpenReader(f, s_.params, &reader), WT_OK);
    YuvaImage out;
    ASSERT_EQ(out.Allocate(40, 24, true), WT_OK);
    EXPECT_EQ(reader->ReadFrame(0, &out), WT_OK);
    EXPECT_EQ(reader->ReadFrame(1, &out), WT_ERR_INVALID_DATA) << at;
    EXPECT_EQ(reader->ReadFrame(1, &out), WT_ERR_INVALID_DATA) << at;
    EXPECT_EQ(reader->ReadFrame(2, &out), WT_OK);
  }
}

TEST_F(WscCorruptionTest, OtherVersionIsUnsupported) {
  Bytes f = file_;
  f[8] = 2;
  FixHeaderCrc(&f);
  std::unique_ptr<WscReader> reader;
  EXPECT_EQ(OpenReader(f, s_.params, &reader), WT_ERR_UNSUPPORTED);
}

TEST_F(WscCorruptionTest, MismatchedExpectationFails) {
  std::unique_ptr<WscReader> reader;
  std::vector<WscParams> wrong(6, s_.params);
  wrong[0].width = 41;
  wrong[1].height = 23;
  wrong[2].has_alpha = false;
  wrong[3].frame_count = 2;
  wrong[4].source_size = 12346;
  wrong[5].source_crc ^= 1;
  for (size_t i = 0; i < wrong.size(); ++i) {
    EXPECT_EQ(OpenReader(file_, wrong[i], &reader), WT_ERR_INVALID_DATA) << i;
  }
  WscParams invalid = s_.params;
  invalid.frame_count = 0;
  EXPECT_EQ(OpenReader(file_, invalid, &reader), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(WscReader::Open(nullptr, s_.params, DefaultLimits(), &reader),
            WT_ERR_INVALID_ARGUMENT);
}

TEST_F(WscCorruptionTest, LimitsApply) {
  wt_limits limits = DefaultLimits();
  std::unique_ptr<WscReader> reader;
  limits.max_dimension = 39;
  EXPECT_EQ(OpenReader(file_, s_.params, &reader, limits), WT_ERR_INVALID_DATA);
  limits = DefaultLimits();
  limits.max_frames = 2;
  EXPECT_EQ(OpenReader(file_, s_.params, &reader, limits), WT_ERR_INVALID_DATA);
}

TEST_F(WscCorruptionTest, ReadFrameRejectsBadArguments) {
  std::unique_ptr<WscReader> reader;
  ASSERT_EQ(OpenReader(file_, s_.params, &reader), WT_OK);
  YuvaImage out;
  ASSERT_EQ(out.Allocate(40, 24, true), WT_OK);
  EXPECT_EQ(reader->ReadFrame(3, &out), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(reader->ReadFrame(0, nullptr), WT_ERR_INVALID_ARGUMENT);
  YuvaImage opaque;
  ASSERT_EQ(opaque.Allocate(40, 24, false), WT_OK);
  EXPECT_EQ(reader->ReadFrame(0, &opaque), WT_ERR_INVALID_ARGUMENT);
  YuvaImage other;
  ASSERT_EQ(other.Allocate(24, 40, true), WT_OK);
  EXPECT_EQ(reader->ReadFrame(0, &other), WT_ERR_INVALID_ARGUMENT);
}

// --- The cache inside Anim ---------------------------------------------------

struct Played {
  std::vector<Bytes> pixels;
  std::vector<wt_frame_info> infos;
  std::vector<wt_status> statuses;
};

// An Anim over a corpus file, holding the source bytes it reads.
struct CorpusAnim {
  std::unique_ptr<ByteSource> source;
  std::unique_ptr<Anim> anim;
};

wt_status MakeAnimFromFile(const std::string& path, uint32_t w, uint32_t h,
                           const std::string& cache_path, CorpusAnim* out) {
  wt_status s = ByteSource::OpenFile(path.c_str(), UINT64_MAX, &out->source);
  if (s != WT_OK) return s;
  const ByteView bytes = out->source->whole();
  return Anim::Create(bytes, wt::Sniff(bytes), DefaultLimits(), w, h,
                      cache_path.empty() ? nullptr : cache_path.c_str(),
                      &out->anim);
}

wt_status MakeAnim(const std::string& name, uint32_t w, uint32_t h,
                   const std::string& cache_path, CorpusAnim* out) {
  return MakeAnimFromFile((kAssets / name).string(), w, h, cache_path, out);
}

// Plays from the cursor to WT_END.
Played PlayToEnd(Anim* anim) {
  Played p;
  const size_t stride = size_t{anim->width()} * 4;
  for (;;) {
    Bytes px(stride * anim->height(), 0xCD);
    wt_frame_info info;
    const wt_status s =
        anim->NextFrame(px.data(), px.size(), static_cast<uint32_t>(stride),
                        WT_PIXEL_FORMAT_RGBA8888, &info);
    if (s == WT_END) break;
    p.statuses.push_back(s);
    p.pixels.push_back(std::move(px));
    p.infos.push_back(info);
  }
  return p;
}

void ExpectSamePlayback(const Played& a, const Played& b) {
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    EXPECT_EQ(a.statuses[i], b.statuses[i]) << i;
    EXPECT_TRUE(a.pixels[i] == b.pixels[i]) << "frame " << i;
    EXPECT_EQ(a.infos[i].index, b.infos[i].index) << i;
    EXPECT_EQ(a.infos[i].duration_ms, b.infos[i].duration_ms) << i;
    EXPECT_EQ(a.infos[i].pts_ms, b.infos[i].pts_ms) << i;
  }
}

// The uncached playback of name at w x h: the reference for everything.
Played Reference(const std::string& name, uint32_t w, uint32_t h) {
  CorpusAnim a;
  EXPECT_EQ(MakeAnim(name, w, h, "", &a), WT_OK);
  if (a.anim == nullptr) return {};
  EXPECT_EQ(a.anim->cache_mode(), Mode::kNone);
  return PlayToEnd(a.anim.get());
}

struct CorpusCase {
  const char* name;
  uint32_t w, h;
};

class AnimCacheCorpusTest : public testing::TestWithParam<CorpusCase> {};

// First pass writes, the loop and a new animation read, and every frame and
// its timing is identical to plain decoding.
TEST_P(AnimCacheCorpusTest, WritesThenReads) {
  const CorpusCase c = GetParam();
  TempDir dir;
  const std::string path = dir.File("c.wsc");
  const Played ref = Reference(c.name, c.w, c.h);
  ASSERT_FALSE(ref.pixels.empty());

  CorpusAnim first;
  ASSERT_EQ(MakeAnim(c.name, c.w, c.h, path, &first), WT_OK);
  EXPECT_EQ(first.anim->cache_mode(), Mode::kPending);
  ExpectSamePlayback(PlayToEnd(first.anim.get()), ref);
  EXPECT_EQ(first.anim->cache_mode(), Mode::kReading);
  EXPECT_EQ(dir.List(), std::vector<std::string>{"c.wsc"});
  // The next loop comes from the file.
  ASSERT_EQ(first.anim->Seek(0), WT_OK);
  ExpectSamePlayback(PlayToEnd(first.anim.get()), ref);

  CorpusAnim second;
  ASSERT_EQ(MakeAnim(c.name, c.w, c.h, path, &second), WT_OK);
  EXPECT_EQ(second.anim->cache_mode(), Mode::kReading);
  ExpectSamePlayback(PlayToEnd(second.anim.get()), ref);
  // Seeking within the cache.
  const uint32_t mid = static_cast<uint32_t>(ref.pixels.size() / 2);
  ASSERT_EQ(second.anim->Seek(mid), WT_OK);
  const Played tail = PlayToEnd(second.anim.get());
  ASSERT_EQ(tail.pixels.size(), ref.pixels.size() - mid);
  EXPECT_TRUE(tail.pixels[0] == ref.pixels[mid]);
  EXPECT_EQ(tail.infos[0].index, mid);
}

INSTANTIATE_TEST_SUITE_P(
    Corpus, AnimCacheCorpusTest,
    testing::Values(CorpusCase{"anim.webp", 0, 0},
                    CorpusCase{"anim.webp", 48, 48},
                    CorpusCase{"anim_disposal.gif", 0, 0},
                    CorpusCase{"anim_disposal.gif", 48, 48},
                    CorpusCase{"vp9_alpha_512.webm", 128, 128},
                    CorpusCase{"vp9_opaque_256.webm", 0, 0},
                    CorpusCase{"vp8_alpha_256.webm", 96, 96},
                    CorpusCase{"vp9_alpha_vfr.webm", 0, 0},
                    CorpusCase{"vp9_odd_301x199.webm", 0, 0},
                    CorpusCase{"vp9_single_frame.webm", 64, 64},
                    CorpusCase{"vp9_with_audio.webm", 0, 0},
                    CorpusCase{"truncated.webm", 64, 64}),
    [](const testing::TestParamInfo<CorpusCase>& info) {
      std::string n = info.param.name;
      n = n.substr(0, n.find('.'));
      return n + "_" + std::to_string(info.param.w);
    });

constexpr const char* kClip = "vp8_alpha_256.webm";
constexpr uint32_t kSize = 64;

class AnimCacheTest : public testing::Test {
 protected:
  void SetUp() override {
    path_ = dir_.File("c.wsc");
    ref_ = Reference(kClip, kSize, kSize);
    ASSERT_FALSE(ref_.pixels.empty());
  }

  // Writes a valid cache file for kClip at kSize.
  void WriteCache() {
    CorpusAnim a;
    ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
    PlayToEnd(a.anim.get());
    ASSERT_TRUE(fs::exists(path_));
  }

  // Opens a new Anim on the (possibly damaged) cache and checks that it
  // deleted the file, decodes correctly and writes a good file again.
  void ExpectRejectedAndRewritten() {
    CorpusAnim a;
    ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
    EXPECT_EQ(a.anim->cache_mode(), Mode::kPending);
    EXPECT_FALSE(fs::exists(path_));
    ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
    EXPECT_EQ(a.anim->cache_mode(), Mode::kReading);
    CorpusAnim b;
    ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &b), WT_OK);
    EXPECT_EQ(b.anim->cache_mode(), Mode::kReading);
    ExpectSamePlayback(PlayToEnd(b.anim.get()), ref_);
  }

  TempDir dir_;
  std::string path_;
  Played ref_;
};

TEST_F(AnimCacheTest, PartialFileIsDeletedAndRedecoded) {
  WriteCache();
  Bytes f = ReadFile(path_);
  f.resize(f.size() / 2);
  WriteFile(path_, f);
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, EmptyFileIsDeletedAndRedecoded) {
  WriteFile(path_, {});
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, DamagedHeaderIsDeletedAndRedecoded) {
  WriteCache();
  Bytes f = ReadFile(path_);
  f[20] ^= 1;  // Height.
  WriteFile(path_, f);
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, OversizedFileIsDeletedAndRedecoded) {
  WriteFile(path_, Bytes(size_t{8} << 20, 0));
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, CacheOfOtherContentIsDeletedAndRedecoded) {
  // A well-formed cache of the right shape whose source fingerprint differs,
  // as when a caller key is reused for new content.
  WriteCache();
  Bytes f = ReadFile(path_);
  f[36] ^= 1;  // source_crc.
  const uint32_t crc = wt::Crc32(0, f.data(), 60);
  for (int i = 0; i < 4; ++i) f[60 + i] = static_cast<uint8_t>(crc >> (8 * i));
  WriteFile(path_, f);
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, CacheWithoutAlphaIsDeletedAndRedecoded) {
  // vp9_opaque_256 has as many frames, but no alpha.
  {
    CorpusAnim other;
    ASSERT_EQ(MakeAnim("vp9_opaque_256.webm", kSize, kSize, path_, &other),
              WT_OK);
    PlayToEnd(other.anim.get());
    ASSERT_TRUE(fs::exists(path_));
  }
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, CacheAtOtherSizeIsDeletedAndRedecoded) {
  {
    CorpusAnim other;
    ASSERT_EQ(MakeAnim(kClip, kSize * 2, kSize * 2, path_, &other), WT_OK);
    PlayToEnd(other.anim.get());
    ASSERT_TRUE(fs::exists(path_));
  }
  ExpectRejectedAndRewritten();
}

TEST_F(AnimCacheTest, DamagedFrameFallsBackToDecoding) {
  WriteCache();
  Bytes f = ReadFile(path_);
  // Find frame 37's bytes from the table (entry offset field).
  const size_t entry = wt::kWscHeaderSize + 37 * wt::kWscEntrySize;
  uint64_t offset = 0;
  for (int i = 0; i < 8; ++i) offset |= uint64_t{f[entry + i]} << (8 * i);
  ASSERT_LT(offset, f.size());
  f[offset + 3] ^= 0x55;
  WriteFile(path_, f);

  CorpusAnim a;
  ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kReading);
  // Frames before 37 come from the cache, 37 on from the decoder; all match.
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kPending);
  EXPECT_FALSE(fs::exists(path_));
  // That pass didn't start at frame 0 with a writer, so the next one writes.
  ASSERT_EQ(a.anim->Seek(0), WT_OK);
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kReading);
  EXPECT_TRUE(fs::exists(path_));
}

TEST_F(AnimCacheTest, SeekDuringFirstPassDefersWriting) {
  CorpusAnim a;
  ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
  Bytes px(size_t{kSize} * kSize * 4);
  for (int i = 0; i < 10; ++i) {
    ASSERT_EQ(a.anim->NextFrame(px.data(), px.size(), kSize * 4,
                                WT_PIXEL_FORMAT_RGBA8888, nullptr),
              WT_OK);
  }
  EXPECT_EQ(a.anim->cache_mode(), Mode::kWriting);
  ASSERT_EQ(a.anim->Seek(30), WT_OK);
  PlayToEnd(a.anim.get());
  EXPECT_EQ(a.anim->cache_mode(), Mode::kPending);
  EXPECT_TRUE(dir_.List().empty());  // No file, no temporary left behind.

  // Seeking back to 0 mid-pass restarts the file from there.
  ASSERT_EQ(a.anim->Seek(0), WT_OK);
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(a.anim->NextFrame(px.data(), px.size(), kSize * 4,
                                WT_PIXEL_FORMAT_RGBA8888, nullptr),
              WT_OK);
  }
  ASSERT_EQ(a.anim->Seek(0), WT_OK);
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kReading);
  EXPECT_EQ(dir_.List(), std::vector<std::string>{"c.wsc"});
}

TEST_F(AnimCacheTest, DestroyMidWriteLeavesNothing) {
  {
    CorpusAnim a;
    ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
    Bytes px(size_t{kSize} * kSize * 4);
    for (int i = 0; i < 10; ++i) {
      ASSERT_EQ(a.anim->NextFrame(px.data(), px.size(), kSize * 4,
                                  WT_PIXEL_FORMAT_RGBA8888, nullptr),
                WT_OK);
    }
    EXPECT_EQ(a.anim->cache_mode(), Mode::kWriting);
    EXPECT_EQ(dir_.List().size(), 1u);  // The temporary file.
  }
  EXPECT_TRUE(dir_.List().empty());
}

TEST_F(AnimCacheTest, UnwritableDirectoryStillPlays) {
  CorpusAnim a;
  ASSERT_EQ(MakeAnim(kClip, kSize, kSize, dir_.File("missing/c.wsc"), &a),
            WT_OK);
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kNone);  // Gave up; no retries.
  ASSERT_EQ(a.anim->Seek(0), WT_OK);
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
}

TEST_F(AnimCacheTest, DirectoryInTheWayStillPlays) {
  fs::create_directory(path_);
  CorpusAnim a;
  ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path_, &a), WT_OK);
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref_);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kNone);  // The rename failed.
  EXPECT_EQ(dir_.List(), std::vector<std::string>{"c.wsc"});
  EXPECT_TRUE(fs::is_directory(path_));
}

// --- File size cap (T6.1, S-04 and S-11, ADR 0026) ---------------------------

TEST(WscCapTest, WriterStopsAtItsCap) {
  TempDir dir;
  const Synthetic s = MakeSynthetic(64, 48, true, 6);
  const std::string path = dir.File("c.wsc");
  ASSERT_EQ(WriteSynthetic(s, path), WT_OK);
  const uint64_t full = fs::file_size(path);
  fs::remove(path);

  // A file of exactly the cap is written.
  std::unique_ptr<WscWriter> w;
  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, full, &w),
            WT_OK);
  EXPECT_EQ(w->max_file_size(), full);
  for (size_t i = 0; i < s.frames.size(); ++i) {
    ASSERT_EQ(w->Append(s.frames[i]->view(), s.durations[i], s.pts[i]), WT_OK);
  }
  ASSERT_EQ(w->Finish(), WT_OK);
  EXPECT_EQ(fs::file_size(path), full);
  fs::remove(path);

  // One byte less, and the last frame is refused: the file is abandoned and
  // its temporary deleted, and the writer only fails from then on.
  ASSERT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, full - 1, &w),
            WT_OK);
  for (size_t i = 0; i + 1 < s.frames.size(); ++i) {
    ASSERT_EQ(w->Append(s.frames[i]->view(), s.durations[i], s.pts[i]), WT_OK);
  }
  EXPECT_EQ(dir.List().size(), 1u);  // The temporary.
  const size_t last = s.frames.size() - 1;
  EXPECT_EQ(w->Append(s.frames[last]->view(), s.durations[last], s.pts[last]),
            WT_ERR_LIMIT);
  EXPECT_TRUE(dir.List().empty());
  EXPECT_EQ(w->Append(s.frames[last]->view(), s.durations[last], s.pts[last]),
            WT_ERR_IO);
  EXPECT_EQ(w->Finish(), WT_ERR_IO);
  EXPECT_TRUE(dir.List().empty());
}

TEST(WscCapTest, AFileThatCantFitAtAnyRatioIsNeverStarted) {
  TempDir dir;
  const Synthetic s = MakeSynthetic(64, 48, true, 6);
  const std::string path = dir.File("c.wsc");
  const uint64_t min = wt::WscMinFileSize(s.params);
  ASSERT_GT(min, 0u);
  std::unique_ptr<WscWriter> w;
  EXPECT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, min - 1, &w),
            WT_ERR_LIMIT);
  EXPECT_EQ(w, nullptr);
  EXPECT_TRUE(dir.List().empty());
  EXPECT_EQ(WscWriter::Create(path.c_str(), s.params, s.color, min, &w), WT_OK);
}

// WscMinFileSize may refuse a write up front only because nothing can
// compress below it: LZ4's best case (all-zero frames at the largest size)
// stays above it.
TEST(WscCapTest, MinFileSizeIsBelowTheBestCompression) {
  TempDir dir;
  for (const bool alpha : {false, true}) {
    for (const uint32_t side : {1u, 16u, 1024u}) {
      WscParams params;
      params.width = side;
      params.height = side;
      params.has_alpha = alpha;
      params.frame_count = 3;
      YuvaImage zero;
      ASSERT_EQ(zero.Allocate(side, side, alpha), WT_OK);
      std::memset(zero.data(), 0, zero.data_size());
      const std::string path = dir.File("z.wsc");
      std::unique_ptr<WscWriter> w;
      ASSERT_EQ(WscWriter::Create(path.c_str(), params, wt::YuvColor(),
                                  UINT64_MAX, &w),
                WT_OK);
      for (uint32_t i = 0; i < params.frame_count; ++i) {
        ASSERT_EQ(w->Append(zero.view(), 10, int64_t{i} * 10), WT_OK);
      }
      ASSERT_EQ(w->Finish(), WT_OK);
      EXPECT_GE(fs::file_size(path), wt::WscMinFileSize(params))
          << side << (alpha ? " alpha" : "");
      EXPECT_LE(fs::file_size(path), wt::WscMaxFileSize(params));
    }
  }
}

TEST(WscCapTest, CapIsAQuarterOfTheBudgetUpTo64MiB) {
  const uint64_t saved = wt_cache_get_budget();
  wt_cache_set_budget(wt::kDefaultCacheBudget);
  EXPECT_EQ(wt::CacheFileCap(), uint64_t{50} << 20);
  wt_cache_set_budget(UINT64_MAX);
  EXPECT_EQ(wt::CacheFileCap(), uint64_t{64} << 20);
  wt_cache_set_budget(uint64_t{1} << 30);
  EXPECT_EQ(wt::CacheFileCap(), uint64_t{64} << 20);
  wt_cache_set_budget(1000);
  EXPECT_EQ(wt::CacheFileCap(), 250u);
  wt_cache_set_budget(0);
  EXPECT_EQ(wt::CacheFileCap(), 0u);
  wt_cache_set_budget(saved);
}

TEST(WscCapTest, RefusalsAreRememberedPerPathAndCap) {
  wt::ForgetRefusedCacheFilesForTesting();
  EXPECT_FALSE(wt::CacheFileRefused("/c/a.wsc", 100));
  wt::RefuseCacheFile("/c/a.wsc", 100);
  EXPECT_TRUE(wt::CacheFileRefused("/c/a.wsc", 100));
  EXPECT_TRUE(wt::CacheFileRefused("/c/a.wsc", 50));
  EXPECT_FALSE(wt::CacheFileRefused("/c/a.wsc", 101));  // The budget grew.
  EXPECT_FALSE(wt::CacheFileRefused("/c/b.wsc", 1));
  // Bounded: the oldest refusal is forgotten first.
  for (size_t i = 0; i < wt::kMaxRefusedCacheFiles; ++i) {
    wt::RefuseCacheFile("/c/n" + std::to_string(i) + ".wsc", 1);
  }
  EXPECT_FALSE(wt::CacheFileRefused("/c/a.wsc", 1));
  EXPECT_TRUE(wt::CacheFileRefused("/c/n0.wsc", 1));
  wt::ForgetRefusedCacheFilesForTesting();
  EXPECT_FALSE(wt::CacheFileRefused("/c/n0.wsc", 1));
}

// The review's crafted file (S-04), made smaller: a 64 x 64 VP9 keyframe of
// noise with alpha, then 599 frames repeating it (10 s at 60 fps, 35 KB).
// ADR 0026 has the ffmpeg commands. Scaled to 512 x 512 (the largest bucket)
// none of its frames compress: uncapped, its cache file was 368 MB
// (WscMaxFileSize: 395 MB), and as the newest file, eviction then deleted
// every other file and then it.
const std::string kNoiseClip =
    (fs::path(WT_FUZZ_CORPUS_DIR) / "webm" / "noise_keyframe_static.webm")
        .string();

// Sets the budget for one test and restores it.
class ScopedBudget {
 public:
  explicit ScopedBudget(uint64_t budget) : saved_(wt_cache_get_budget()) {
    wt_cache_set_budget(budget);
  }
  ~ScopedBudget() { wt_cache_set_budget(saved_); }

 private:
  uint64_t saved_;
};

struct FileState {
  uint64_t size = 0;
  int64_t mtime_ns = 0;
  uint64_t inode = 0;
};

FileState StatFile(const std::string& path) {
  struct stat st;
  FileState f;
  if (stat(path.c_str(), &st) != 0) return f;
  f.size = static_cast<uint64_t>(st.st_size);
#ifdef __APPLE__
  f.mtime_ns =
      int64_t{st.st_mtimespec.tv_sec} * 1000000000 + st.st_mtimespec.tv_nsec;
#else
  f.mtime_ns = int64_t{st.st_mtim.tv_sec} * 1000000000 + st.st_mtim.tv_nsec;
#endif
  f.inode = static_cast<uint64_t>(st.st_ino);
  return f;
}

// The largest writer temporary in dir (0: none).
uint64_t LargestTemp(const TempDir& dir) {
  uint64_t largest = 0;
  for (const std::string& n : dir.List()) {
    if (n.find(".tmp") != std::string::npos) {
      largest = std::max<uint64_t>(largest, fs::file_size(dir.path() / n));
    }
  }
  return largest;
}

uint64_t Fnv1a(const Bytes& b) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
  return h;
}

TEST(AnimCacheCapTest, CraftedFileStopsAtTheCapAndEvictsNothing) {
  wt::ForgetRefusedCacheFilesForTesting();
  ScopedBudget budget(wt::kDefaultCacheBudget);
  const uint64_t cap = wt::CacheFileCap();
  ASSERT_EQ(cap, uint64_t{50} << 20);
  TempDir dir;

  // Other stickers' cache files, which the write must not push out.
  const std::vector<std::string> others = {"a.wsc", "b.wsc", "c.wsc"};
  for (size_t i = 0; i < others.size(); ++i) {
    CorpusAnim a;
    ASSERT_EQ(
        MakeAnim(kClip, kSize * static_cast<uint32_t>(i + 1),
                 kSize * static_cast<uint32_t>(i + 1), dir.File(others[i]), &a),
        WT_OK);
    PlayToEnd(a.anim.get());
  }
  ASSERT_EQ(dir.List().size(), others.size());
  std::vector<FileState> before;
  for (const std::string& n : others) before.push_back(StatFile(dir.File(n)));

  const std::string path = dir.File("noise.wsc");
  CorpusAnim crafted;
  ASSERT_EQ(MakeAnimFromFile(kNoiseClip, 512, 512, path, &crafted), WT_OK);
  Anim* anim = crafted.anim.get();
  ASSERT_EQ(anim->info().frame_count, 600u);
  wt::WscParams params;
  params.width = 512;
  params.height = 512;
  params.has_alpha = true;
  params.frame_count = 600;
  EXPECT_GT(wt::WscMaxFileSize(params), uint64_t{390} * 1000 * 1000);

  // One pass: every frame plays, the temporary never passes the cap, and
  // it is gone once the cap is reached.
  Bytes px(size_t{512} * 512 * 4);
  std::vector<uint64_t> first_loop;
  uint64_t peak = 0;
  for (uint32_t i = 0; i < 600; ++i) {
    ASSERT_EQ(anim->NextFrame(px.data(), px.size(), 512 * 4,
                              WT_PIXEL_FORMAT_RGBA8888, nullptr),
              WT_OK)
        << i;
    first_loop.push_back(Fnv1a(px));
    peak = std::max(peak, LargestTemp(dir));
  }
  EXPECT_LE(peak, cap);
  // It really got there: within one incompressible frame of the cap.
  EXPECT_GT(peak, cap - wt::WscFrameBytes(512, 512, true) * 2);
  EXPECT_EQ(anim->cache_mode(), Mode::kNone);
  // No file, no temporary, and every other file untouched.
  EXPECT_EQ(dir.List(), others) << "noise.wsc or a temporary was left";
  for (size_t i = 0; i < others.size(); ++i) {
    const FileState now = StatFile(dir.File(others[i]));
    EXPECT_EQ(now.size, before[i].size) << others[i];
    EXPECT_EQ(now.mtime_ns, before[i].mtime_ns) << others[i];
    EXPECT_EQ(now.inode, before[i].inode) << others[i];
  }

  // It still plays, by decoding, and never writes again.
  ASSERT_EQ(anim->Seek(0), WT_OK);
  for (uint32_t i = 0; i < 120; ++i) {
    ASSERT_EQ(anim->NextFrame(px.data(), px.size(), 512 * 4,
                              WT_PIXEL_FORMAT_RGBA8888, nullptr),
              WT_OK);
    EXPECT_EQ(Fnv1a(px), first_loop[i]) << i;
    ASSERT_EQ(LargestTemp(dir), 0u) << i;
  }
  EXPECT_EQ(anim->cache_mode(), Mode::kNone);

  // Neither does another animation of the same sticker at that size.
  CorpusAnim again;
  ASSERT_EQ(MakeAnimFromFile(kNoiseClip, 512, 512, path, &again), WT_OK);
  EXPECT_EQ(again.anim->cache_mode(), Mode::kPending);
  ASSERT_EQ(again.anim->NextFrame(px.data(), px.size(), 512 * 4,
                                  WT_PIXEL_FORMAT_RGBA8888, nullptr),
            WT_OK);
  EXPECT_EQ(Fnv1a(px), first_loop[0]);
  EXPECT_EQ(again.anim->cache_mode(), Mode::kNone);
  EXPECT_EQ(dir.List(), others);
}

// Every corpus clip is still cached at the largest bucket (512 on the longer
// side, as StickerBuckets.forSize), under the default budget's cap.
TEST(AnimCacheCapTest, CorpusIsStillCachedAtTheLargestBucket) {
  wt::ForgetRefusedCacheFilesForTesting();
  ScopedBudget budget(wt::kDefaultCacheBudget);
  struct Clip {
    const char* name;
    uint32_t w, h;
  };
  const Clip clips[] = {
      {"anim.webp", 512, 512},
      {"anim_disposal.gif", 512, 512},
      {"vp9_alpha_512.webm", 512, 512},
      {"vp9_opaque_256.webm", 512, 512},
      {"vp8_alpha_256.webm", 512, 512},
      {"vp9_alpha_vfr.webm", 512, 512},
      {"vp9_odd_301x199.webm", 512, 338},
      {"vp9_single_frame.webm", 512, 512},
      {"vp9_with_audio.webm", 512, 512},
      {"truncated.webm", 512, 512},
  };
  TempDir dir;
  for (const Clip& c : clips) {
    SCOPED_TRACE(c.name);
    const std::string path = dir.File(std::string(c.name) + ".wsc");
    {
      CorpusAnim a;
      ASSERT_EQ(MakeAnim(c.name, c.w, c.h, path, &a), WT_OK);
      Bytes px(size_t{c.w} * c.h * 4);
      while (a.anim->NextFrame(px.data(), px.size(), c.w * 4,
                               WT_PIXEL_FORMAT_RGBA8888, nullptr) == WT_OK) {
      }
      EXPECT_EQ(a.anim->cache_mode(), Mode::kReading);
    }
    ASSERT_TRUE(fs::exists(path));
    EXPECT_LE(fs::file_size(path), wt::CacheFileCap());
    CorpusAnim b;
    ASSERT_EQ(MakeAnim(c.name, c.w, c.h, path, &b), WT_OK);
    EXPECT_EQ(b.anim->cache_mode(), Mode::kReading);
  }
}

// S-11: a file that can't be mapped once written stops caching for that
// animation, instead of being written again every loop.
TEST(AnimCacheCapTest, AFailedReopenStopsCaching) {
  TempDir dir;
  const std::string path = dir.File("c.wsc");
  const Played ref = Reference(kClip, kSize, kSize);
  CorpusAnim a;
  ASSERT_EQ(MakeAnim(kClip, kSize, kSize, path, &a), WT_OK);
  a.anim->FailCacheReopenForTesting();
  ExpectSamePlayback(PlayToEnd(a.anim.get()), ref);
  EXPECT_EQ(a.anim->cache_mode(), Mode::kNone);
  ASSERT_TRUE(fs::exists(path));  // Written, just not mapped.
  const FileState written = StatFile(path);
  for (int loop = 0; loop < 2; ++loop) {
    ASSERT_EQ(a.anim->Seek(0), WT_OK);
    ExpectSamePlayback(PlayToEnd(a.anim.get()), ref);
  }
  const FileState now = StatFile(path);
  EXPECT_EQ(now.inode, written.inode) << "written again";
  EXPECT_EQ(now.mtime_ns, written.mtime_ns) << "written again";
  EXPECT_EQ(dir.List(), std::vector<std::string>{"c.wsc"});
}

// --- Through the C ABI -------------------------------------------------------

TEST(AnimCacheApiTest, CreateCached) {
  TempDir dir;
  const std::string path = dir.File("c.wsc");
  wt_test::Source source;
  ASSERT_EQ(wt_test::OpenSource(kClip, &source), WT_OK);

  wt_test::Anim plain;
  ASSERT_EQ(wt_anim_create(source.p, kSize, kSize, &plain.p), WT_OK);
  std::vector<Bytes> ref;
  Bytes px;
  while (wt_test::Next(plain, &px) == WT_OK) ref.push_back(px);

  // NULL and "" mean no cache.
  for (const char* none : {static_cast<const char*>(nullptr), ""}) {
    wt_test::Anim anim;
    ASSERT_EQ(wt_anim_create_cached(source.p, kSize, kSize, none, &anim.p),
              WT_OK);
    size_t n = 0;
    while (wt_test::Next(anim, &px) == WT_OK) EXPECT_TRUE(px == ref[n++]);
    EXPECT_EQ(n, ref.size());
    EXPECT_TRUE(dir.List().empty());
  }

  for (int round = 0; round < 2; ++round) {  // Writes, then reads.
    wt_test::Anim anim;
    ASSERT_EQ(
        wt_anim_create_cached(source.p, kSize, kSize, path.c_str(), &anim.p),
        WT_OK);
    size_t n = 0;
    while (wt_test::Next(anim, &px) == WT_OK) EXPECT_TRUE(px == ref[n++]);
    EXPECT_EQ(n, ref.size());
    EXPECT_TRUE(fs::exists(path));
  }

  wt_anim* out = reinterpret_cast<wt_anim*>(1);
  EXPECT_EQ(wt_anim_create_cached(nullptr, kSize, kSize, path.c_str(), &out),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(out, nullptr);
  EXPECT_EQ(wt_anim_create_cached(source.p, kSize, 0, path.c_str(), &out),
            WT_ERR_INVALID_ARGUMENT);
}

}  // namespace
