// Tests for byte sources (src/demux/source.*) and format sniffing
// (src/demux/sniff.*), including every file in test_assets/.
#include "demux/source.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "demux/sniff.h"
#include "gtest/gtest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

const fs::path kAssets = WT_TEST_ASSETS_DIR;

std::vector<uint8_t> ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>());
}

wt::ByteView View(const std::vector<uint8_t>& v) {
  return wt::ByteView{v.data(), v.size()};
}

// Format each corpus file must sniff as. A file added to test_assets/ without
// a row here fails CorpusIsFullyCovered.
const std::map<std::string, wt_format>& CorpusFormats() {
  static const std::map<std::string, wt_format> formats = {
      {"anim.webp", WT_FORMAT_WEBP},
      {"anim_disposal.gif", WT_FORMAT_GIF},
      {"garbage.bin", WT_FORMAT_UNKNOWN},
      {"truncated.webm", WT_FORMAT_WEBM},
      {"vp8_alpha_256.webm", WT_FORMAT_WEBM},
      {"vp9_2048.webm", WT_FORMAT_WEBM},
      {"vp9_alpha_512.webm", WT_FORMAT_WEBM},
      {"vp9_alpha_vfr.webm", WT_FORMAT_WEBM},
      {"vp9_odd_301x199.webm", WT_FORMAT_WEBM},
      {"vp9_opaque_256.webm", WT_FORMAT_WEBM},
      {"vp9_single_frame.webm", WT_FORMAT_WEBM},
      {"vp9_with_audio.webm", WT_FORMAT_WEBM},
  };
  return formats;
}

const char* KindName(wt_format format) {
  switch (format) {
    case WT_FORMAT_WEBM:
      return "webm";
    case WT_FORMAT_WEBP:
      return "webp";
    case WT_FORMAT_GIF:
      return "gif";
    default:
      return "unknown";
  }
}

// A scratch directory removed at the end of the test.
class TempDir {
 public:
  TempDir() {
    std::string tmpl =
        (fs::temp_directory_path() / "wt_source_XXXXXX").string();
    EXPECT_NE(mkdtemp(tmpl.data()), nullptr);
    path_ = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// --- Sniff -----------------------------------------------------------------

TEST(SniffTest, EmptyAndTinyInputs) {
  EXPECT_EQ(wt::Sniff(wt::ByteView{}), WT_FORMAT_UNKNOWN);
  const std::vector<uint8_t> one = {0x1A};
  EXPECT_EQ(wt::Sniff(View(one)), WT_FORMAT_UNKNOWN);
  const std::vector<uint8_t> zero = {0x00};
  EXPECT_EQ(wt::Sniff(View(zero)), WT_FORMAT_UNKNOWN);
}

TEST(SniffTest, EveryPrefixOfEachMagicIsUnknown) {
  const std::vector<std::vector<uint8_t>> full = {
      {0x1A, 0x45, 0xDF, 0xA3},
      {'R', 'I', 'F', 'F', 1, 0, 0, 0, 'W', 'E', 'B', 'P'},
      {'G', 'I', 'F', '8', '9', 'a'},
  };
  for (const auto& magic : full) {
    for (size_t n = 0; n < magic.size(); ++n) {
      EXPECT_EQ(wt::Sniff(wt::ByteView{magic.data(), n}), WT_FORMAT_UNKNOWN)
          << "prefix length " << n;
    }
    EXPECT_NE(wt::Sniff(View(magic)), WT_FORMAT_UNKNOWN);
  }
}

TEST(SniffTest, Signatures) {
  const std::vector<uint8_t> webm = {0x1A, 0x45, 0xDF, 0xA3};
  EXPECT_EQ(wt::Sniff(View(webm)), WT_FORMAT_WEBM);
  const std::vector<uint8_t> gif87 = {'G', 'I', 'F', '8', '7', 'a'};
  EXPECT_EQ(wt::Sniff(View(gif87)), WT_FORMAT_GIF);
  const std::vector<uint8_t> gif89 = {'G', 'I', 'F', '8', '9', 'a'};
  EXPECT_EQ(wt::Sniff(View(gif89)), WT_FORMAT_GIF);
  const std::vector<uint8_t> webp = {'R',  'I',  'F', 'F', 0xFF, 0xFF,
                                     0xFF, 0xFF, 'W', 'E', 'B',  'P'};
  EXPECT_EQ(wt::Sniff(View(webp)), WT_FORMAT_WEBP);
}

TEST(SniffTest, NearMissesAreUnknown) {
  const std::vector<uint8_t> wav = {'R', 'I', 'F', 'F', 0,   0,
                                    0,   0,   'W', 'A', 'V', 'E'};
  EXPECT_EQ(wt::Sniff(View(wav)), WT_FORMAT_UNKNOWN);
  const std::vector<uint8_t> gif_bad = {'G', 'I', 'F', '8', '8', 'a'};
  EXPECT_EQ(wt::Sniff(View(gif_bad)), WT_FORMAT_UNKNOWN);
  const std::vector<uint8_t> lower = {'g', 'i', 'f', '8', '9', 'a'};
  EXPECT_EQ(wt::Sniff(View(lower)), WT_FORMAT_UNKNOWN);
  const std::vector<uint8_t> png = {0x89, 'P',  'N',  'G',
                                    0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_EQ(wt::Sniff(View(png)), WT_FORMAT_UNKNOWN);
}

TEST(SniffTest, EveryByteValueAsFirstByte) {
  // Single-byte and short inputs never read out of bounds (ASan checks).
  for (int b = 0; b < 256; ++b) {
    const uint8_t byte = static_cast<uint8_t>(b);
    EXPECT_EQ(wt::Sniff(wt::ByteView{&byte, 1}), WT_FORMAT_UNKNOWN);
  }
}

// --- ByteSource ------------------------------------------------------------

TEST(ByteSourceTest, MemoryCopiesInput) {
  std::vector<uint8_t> bytes = {1, 2, 3, 4};
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(
      wt::ByteSource::CreateMemory(bytes.data(), bytes.size(), 100, &source),
      WT_OK);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(source->size(), 4u);
  bytes[0] = 99;  // The caller may reuse its buffer.
  EXPECT_EQ(source->whole().data[0], 1);
  EXPECT_NE(source->whole().data, bytes.data());
}

TEST(ByteSourceTest, MemoryLimitChecksBeforeCopy) {
  const std::vector<uint8_t> bytes(11, 0);
  std::unique_ptr<wt::ByteSource> source;
  EXPECT_EQ(
      wt::ByteSource::CreateMemory(bytes.data(), bytes.size(), 10, &source),
      WT_ERR_LIMIT);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(wt::ByteSource::CreateMemory(bytes.data(), 10, 10, &source), WT_OK);
  // A huge size with a bad pointer must fail on the limit, not read memory.
  EXPECT_EQ(wt::ByteSource::CreateMemory(bytes.data(), SIZE_MAX, 10, &source),
            WT_ERR_LIMIT);
  EXPECT_EQ(source, nullptr);
}

TEST(ByteSourceTest, MemoryEmpty) {
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(wt::ByteSource::CreateMemory(nullptr, 0, 10, &source), WT_OK);
  EXPECT_EQ(source->size(), 0u);
  EXPECT_EQ(wt::Sniff(source->whole()), WT_FORMAT_UNKNOWN);
  wt::ByteView view;
  EXPECT_TRUE(source->Slice(0, 0, &view));
  EXPECT_FALSE(source->Slice(0, 1, &view));
  EXPECT_FALSE(source->Slice(1, 0, &view));
}

TEST(ByteSourceTest, MemoryArgumentChecks) {
  const uint8_t byte = 0;
  std::unique_ptr<wt::ByteSource> source;
  EXPECT_EQ(wt::ByteSource::CreateMemory(nullptr, 1, 10, &source),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ByteSource::CreateMemory(&byte, 1, 10, nullptr),
            WT_ERR_INVALID_ARGUMENT);
}

TEST(ByteSourceTest, SliceBounds) {
  const std::vector<uint8_t> bytes = {10, 11, 12, 13, 14};
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(
      wt::ByteSource::CreateMemory(bytes.data(), bytes.size(), 100, &source),
      WT_OK);
  wt::ByteView view;
  ASSERT_TRUE(source->Slice(1, 3, &view));
  EXPECT_EQ(view.size, 3u);
  EXPECT_EQ(view.data[0], 11);
  EXPECT_TRUE(source->Slice(0, 5, &view));
  EXPECT_TRUE(source->Slice(5, 0, &view)) << "empty slice at the end";
  EXPECT_EQ(view.size, 0u);

  const uint64_t kMax = UINT64_MAX;
  EXPECT_FALSE(source->Slice(0, 6, &view));
  EXPECT_EQ(view.size, 0u) << "out is emptied on failure";
  EXPECT_EQ(view.data, nullptr);
  EXPECT_FALSE(source->Slice(6, 0, &view));
  EXPECT_FALSE(source->Slice(4, 2, &view));
  // offset + length wraps to a small number: must still be rejected.
  EXPECT_FALSE(source->Slice(kMax, 2, &view));
  EXPECT_FALSE(source->Slice(2, kMax, &view));
  EXPECT_FALSE(source->Slice(kMax, kMax, &view));
  EXPECT_FALSE(source->Slice(kMax - 1, 3, &view));
  EXPECT_TRUE(source->Slice(0, 0, nullptr)) << "null out only validates";
  EXPECT_FALSE(source->Slice(9, 0, nullptr));
}

TEST(ByteSourceTest, ReadBounds) {
  const std::vector<uint8_t> bytes = {10, 11, 12, 13, 14};
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(
      wt::ByteSource::CreateMemory(bytes.data(), bytes.size(), 100, &source),
      WT_OK);
  uint8_t dst[8];
  std::fill(std::begin(dst), std::end(dst), uint8_t{0xEE});
  ASSERT_TRUE(source->Read(2, 3, dst));
  EXPECT_EQ(dst[0], 12);
  EXPECT_EQ(dst[2], 14);
  EXPECT_EQ(dst[3], 0xEE) << "no write past length";

  std::fill(std::begin(dst), std::end(dst), uint8_t{0xEE});
  EXPECT_FALSE(source->Read(3, 3, dst));
  EXPECT_FALSE(source->Read(UINT64_MAX, 2, dst));
  EXPECT_FALSE(source->Read(1, SIZE_MAX, dst));
  for (uint8_t b : dst) EXPECT_EQ(b, 0xEE) << "dst untouched on failure";
  EXPECT_TRUE(source->Read(5, 0, nullptr));
}

TEST(ByteSourceTest, FileMatchesMemory) {
  TempDir dir;
  const std::vector<uint8_t> bytes = {'G', 'I', 'F', '8', '9', 'a', 7, 8, 9};
  const fs::path path = dir.path() / "x.gif";
  WriteFile(path, bytes);
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(wt::ByteSource::OpenFile(path.c_str(), 100, &source), WT_OK);
  ASSERT_EQ(source->size(), bytes.size());
  EXPECT_EQ(std::vector<uint8_t>(source->whole().data,
                                 source->whole().data + source->size()),
            bytes);
  EXPECT_EQ(wt::Sniff(source->whole()), WT_FORMAT_GIF);
  wt::ByteView view;
  EXPECT_FALSE(source->Slice(0, bytes.size() + 1, &view));
}

TEST(ByteSourceTest, FileStaysValidAfterUnlink) {
  TempDir dir;
  const std::vector<uint8_t> bytes(5000, 0x5A);
  const fs::path path = dir.path() / "x.bin";
  WriteFile(path, bytes);
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(wt::ByteSource::OpenFile(path.c_str(), 10000, &source), WT_OK);
  fs::remove(path);  // The mapping, not the path, holds the data.
  EXPECT_EQ(source->whole().data[4999], 0x5A);
}

TEST(ByteSourceTest, FileEmpty) {
  TempDir dir;
  const fs::path path = dir.path() / "empty";
  WriteFile(path, {});
  std::unique_ptr<wt::ByteSource> source;
  ASSERT_EQ(wt::ByteSource::OpenFile(path.c_str(), 10, &source), WT_OK);
  EXPECT_EQ(source->size(), 0u);
  EXPECT_EQ(wt::Sniff(source->whole()), WT_FORMAT_UNKNOWN);
}

TEST(ByteSourceTest, FileLimit) {
  TempDir dir;
  const fs::path path = dir.path() / "big";
  WriteFile(path, std::vector<uint8_t>(11, 1));
  std::unique_ptr<wt::ByteSource> source;
  EXPECT_EQ(wt::ByteSource::OpenFile(path.c_str(), 10, &source), WT_ERR_LIMIT);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(wt::ByteSource::OpenFile(path.c_str(), 11, &source), WT_OK);
}

TEST(ByteSourceTest, FileErrors) {
  TempDir dir;
  std::unique_ptr<wt::ByteSource> source;
  EXPECT_EQ(
      wt::ByteSource::OpenFile((dir.path() / "missing").c_str(), 10, &source),
      WT_ERR_IO);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(wt::ByteSource::OpenFile(dir.path().c_str(), 10, &source),
            WT_ERR_IO)
      << "a directory is not a sticker";
  const fs::path fifo = dir.path() / "fifo";
  ASSERT_EQ(mkfifo(fifo.c_str(), 0600), 0);
  EXPECT_EQ(wt::ByteSource::OpenFile(fifo.c_str(), 10, &source), WT_ERR_IO)
      << "a FIFO must be rejected without blocking";
  EXPECT_EQ(wt::ByteSource::OpenFile("", 10, &source), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ByteSource::OpenFile(nullptr, 10, &source),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::ByteSource::OpenFile("x", 10, nullptr),
            WT_ERR_INVALID_ARGUMENT);
}

// --- Corpus ----------------------------------------------------------------

TEST(CorpusTest, CorpusIsFullyCovered) {
  std::vector<std::string> on_disk;
  for (const auto& entry : fs::directory_iterator(kAssets)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    if (name == "README.md" || name == "manifest.json") continue;
    on_disk.push_back(name);
  }
  ASSERT_FALSE(on_disk.empty());
  for (const auto& name : on_disk) {
    EXPECT_EQ(CorpusFormats().count(name), 1u) << "no sniff row for " << name;
  }
  for (const auto& [name, format] : CorpusFormats()) {
    EXPECT_TRUE(fs::exists(kAssets / name)) << "missing corpus file " << name;
  }
}

TEST(CorpusTest, TableMatchesManifestKinds) {
  std::ifstream in(kAssets / "manifest.json");
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string manifest = ss.str();
  ASSERT_FALSE(manifest.empty());
  for (const auto& [name, format] : CorpusFormats()) {
    const size_t at = manifest.find("\"name\": \"" + name + "\"");
    ASSERT_NE(at, std::string::npos) << name;
    const std::string key = "\"kind\": \"";
    const size_t kind_at = manifest.find(key, at);
    ASSERT_NE(kind_at, std::string::npos) << name;
    const size_t begin = kind_at + key.size();
    const std::string kind =
        manifest.substr(begin, manifest.find('"', begin) - begin);
    EXPECT_EQ(kind, KindName(format)) << name;
  }
}

TEST(CorpusTest, SniffAndSourcesForEveryFile) {
  for (const auto& [name, format] : CorpusFormats()) {
    SCOPED_TRACE(name);
    const fs::path path = kAssets / name;
    const std::vector<uint8_t> expected = ReadAll(path);
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(wt::Sniff(View(expected)), format);

    std::unique_ptr<wt::ByteSource> mem;
    ASSERT_EQ(wt::ByteSource::CreateMemory(expected.data(), expected.size(),
                                           uint64_t{2} << 20, &mem),
              WT_OK);
    std::unique_ptr<wt::ByteSource> file;
    ASSERT_EQ(wt::ByteSource::OpenFile(path.c_str(), uint64_t{2} << 20, &file),
              WT_OK);
    for (const wt::ByteSource* s : {mem.get(), file.get()}) {
      ASSERT_EQ(s->size(), expected.size());
      EXPECT_EQ(wt::Sniff(s->whole()), format);
      EXPECT_TRUE(
          std::equal(expected.begin(), expected.end(), s->whole().data));
    }
  }
}

TEST(CorpusTest, TruncatedPrefixesNeverCrashSniff) {
  for (const auto& [name, format] : CorpusFormats()) {
    const std::vector<uint8_t> bytes = ReadAll(kAssets / name);
    for (size_t n = 0; n <= 16 && n <= bytes.size(); ++n) {
      wt::Sniff(wt::ByteView{bytes.data(), n});
    }
  }
}

// --- C ABI -----------------------------------------------------------------

TEST(ApiSourceTest, CreatesFromMemoryAndFile) {
  const fs::path path = kAssets / "anim_disposal.gif";
  const std::vector<uint8_t> bytes = ReadAll(path);
  wt_source* from_mem = nullptr;
  ASSERT_EQ(
      wt_source_create_memory(bytes.data(), bytes.size(), nullptr, &from_mem),
      WT_OK);
  EXPECT_NE(from_mem, nullptr);
  wt_source* from_file = nullptr;
  ASSERT_EQ(wt_source_create_file(path.c_str(), nullptr, &from_file), WT_OK);
  EXPECT_NE(from_file, nullptr);
  wt_source_destroy(from_mem);
  wt_source_destroy(from_file);
}

TEST(ApiSourceTest, LimitAndIoErrors) {
  wt_limits limits;
  wt_limits_init_default(&limits);
  limits.max_bytes = 8;
  const std::vector<uint8_t> bytes(9, 0);
  wt_source* source = reinterpret_cast<wt_source*>(uintptr_t{1});
  EXPECT_EQ(
      wt_source_create_memory(bytes.data(), bytes.size(), &limits, &source),
      WT_ERR_LIMIT);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(
      wt_source_create_file((kAssets / "anim.webp").c_str(), &limits, &source),
      WT_ERR_LIMIT);
  EXPECT_EQ(source, nullptr);
  EXPECT_EQ(wt_source_create_file((kAssets / "nope").c_str(), nullptr, &source),
            WT_ERR_IO);
  EXPECT_EQ(source, nullptr);
}

TEST(ApiSourceTest, EmptyAndGarbageMemoryAreValidSources) {
  // Whether the bytes are playable is probe's call (T1.5), not creation's.
  wt_source* source = nullptr;
  ASSERT_EQ(wt_source_create_memory(nullptr, 0, nullptr, &source), WT_OK);
  wt_source_destroy(source);
  const uint8_t one = 0x42;
  ASSERT_EQ(wt_source_create_memory(&one, 1, nullptr, &source), WT_OK);
  wt_source_destroy(source);
}

}  // namespace
