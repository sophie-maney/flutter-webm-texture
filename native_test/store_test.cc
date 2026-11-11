// Tests for the cache directory policy (T3.2): SHA-256 and file naming, stats,
// LRU eviction under a budget, stray temporaries, clearing, and the budget
// being enforced by the core after each write, including two animations
// writing at once.
#include "cache/store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "anim_util.h"
#include "cache/sha256.h"
#include "gtest/gtest.h"
#include "wt_api.h"

namespace fs = std::filesystem;

namespace {

using wt::CacheStats;

std::string Hex(const uint8_t* d, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += kHex[d[i] >> 4];
    s += kHex[d[i] & 15];
  }
  return s;
}

std::string Sha(const std::string& s) {
  uint8_t out[wt::kSha256Size];
  wt::Sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
  return Hex(out, sizeof(out));
}

// Restores the process-wide budget after a test changes it.
class BudgetGuard {
 public:
  BudgetGuard() : saved_(wt_cache_get_budget()) {}
  ~BudgetGuard() { wt_cache_set_budget(saved_); }

 private:
  uint64_t saved_;
};

class TempDir {
 public:
  TempDir() {
    const testing::TestInfo* t =
        testing::UnitTest::GetInstance()->current_test_info();
    path_ = fs::temp_directory_path() /
            ("wt_store_test_" + std::to_string(getpid()) + "_" +
             t->test_suite_name() + "_" + t->name());
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  std::string str() const { return path_.string(); }
  std::string File(const std::string& name) const {
    return (path_ / name).string();
  }

  // Creates name with size bytes, last used age_s seconds ago.
  void Make(const std::string& name, size_t size, int64_t age_s) const {
    std::ofstream(File(name), std::ios::binary) << std::string(size, 'x');
    Age(name, age_s);
  }
  void Age(const std::string& name, int64_t age_s) const {
    struct timespec t[2];
    t[0].tv_sec = t[1].tv_sec = time(nullptr) - age_s;
    t[0].tv_nsec = t[1].tv_nsec = 0;
    ASSERT_EQ(utimensat(AT_FDCWD, File(name).c_str(), t, 0), 0);
  }
  std::set<std::string> List() const {
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator(path_)) {
      names.insert(e.path().filename().string());
    }
    return names;
  }

 private:
  fs::path path_;
};

using Names = std::set<std::string>;

// --- Naming ------------------------------------------------------------------

TEST(Sha256Test, KnownVectors) {
  EXPECT_EQ(Sha(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(Sha("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(Sha(std::string(1000000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// The padding changes shape at 55/56 and 63/64 bytes (digests from Python's
// hashlib).
TEST(Sha256Test, PaddingBoundaries) {
  EXPECT_EQ(Sha(std::string(55, 'a')),
            "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  EXPECT_EQ(Sha(std::string(56, 'a')),
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  EXPECT_EQ(Sha(std::string(64, 'a')),
            "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST(CacheFilePathTest, FormatAndKeys) {
  std::string a, b, c;
  ASSERT_EQ(wt::CacheFilePath("/d", "sha1:abc", 120, 96, &a), WT_OK);
  EXPECT_EQ(a.rfind("/d/", 0), 0u);
  EXPECT_EQ(a.substr(a.size() - 14), "_120x96_v1.wsc");
  EXPECT_EQ(a.size(), std::string("/d/").size() + 32 + 14);
  ASSERT_EQ(wt::CacheFilePath("/d/", "sha1:abc", 120, 96, &b), WT_OK);
  EXPECT_EQ(a, b);  // A trailing slash makes no difference.
  ASSERT_EQ(wt::CacheFilePath("/d", "sha1:abc", 96, 120, &c), WT_OK);
  EXPECT_NE(a, c);
  ASSERT_EQ(wt::CacheFilePath("/d", "key:../../etc/passwd\n", 8, 8, &c), WT_OK);
  EXPECT_EQ(c.find('/', 3), std::string::npos);  // The key can't leave dir.
  EXPECT_EQ(wt::CacheFilePath("", "k", 8, 8, &c), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt::CacheFilePath("/d", "k", 0, 8, &c), WT_ERR_INVALID_ARGUMENT);
}

TEST(CacheFilePathTest, CAbi) {
  char buf[256];
  ASSERT_EQ(wt_cache_file_path("/d", "k", 64, 32, buf, sizeof(buf)), WT_OK);
  std::string expect;
  ASSERT_EQ(wt::CacheFilePath("/d", "k", 64, 32, &expect), WT_OK);
  EXPECT_EQ(std::string(buf), expect);
  // Exactly fitting, then one byte short.
  char tight[256];
  ASSERT_EQ(wt_cache_file_path("/d", "k", 64, 32, tight, expect.size() + 1),
            WT_OK);
  EXPECT_EQ(wt_cache_file_path("/d", "k", 64, 32, tight, expect.size()),
            WT_ERR_LIMIT);
  EXPECT_EQ(tight[0], '\0');
  EXPECT_EQ(wt_cache_file_path(nullptr, "k", 1, 1, buf, sizeof(buf)),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_file_path("/d", nullptr, 1, 1, buf, sizeof(buf)),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_file_path("", "k", 1, 1, buf, sizeof(buf)),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_file_path("/d", "k", 0, 1, buf, sizeof(buf)),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_file_path("/d", "k", 1, 1, nullptr, 8),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_file_path("/d", "k", 1, 1, buf, 0),
            WT_ERR_INVALID_ARGUMENT);
}

// --- Stats and eviction ------------------------------------------------------

TEST(CacheStoreTest, StatsCountOnlyCacheFiles) {
  TempDir dir;
  dir.Make("a.wsc", 100, 0);
  dir.Make("b.wsc", 50, 0);
  dir.Make("a.wsc.123.0.tmp", 1000, 0);  // A writer's temporary.
  dir.Make("notes.txt", 1000, 0);
  dir.Make("x.wsc.bak", 1000, 0);
  dir.Make(".wsc", 1000, 0);  // No stem: not ours.
  fs::create_directory(dir.File("sub.wsc"));
  fs::create_symlink(dir.File("a.wsc"), dir.File("link.wsc"));
  CacheStats stats;
  ASSERT_EQ(wt::CacheDirStats(dir.str(), &stats), WT_OK);
  EXPECT_EQ(stats.file_count, 2u);
  EXPECT_EQ(stats.total_bytes, 150u);

  wt_cache_stats c{};
  ASSERT_EQ(wt_cache_get_stats(dir.str().c_str(), &c), WT_OK);
  EXPECT_EQ(c.file_count, 2u);
  EXPECT_EQ(c.total_bytes, 150u);
}

TEST(CacheStoreTest, MissingOrBadDirectory) {
  TempDir dir;
  wt_cache_stats c{1, 1};
  const std::string gone = dir.File("nope");
  EXPECT_EQ(wt_cache_get_stats(gone.c_str(), &c), WT_OK);
  EXPECT_EQ(c.file_count, 0u);
  EXPECT_EQ(wt_cache_evict(gone.c_str(), &c), WT_OK);
  EXPECT_EQ(wt_cache_clear(gone.c_str(), &c), WT_OK);
  dir.Make("file", 1, 0);  // A file where a directory should be.
  EXPECT_EQ(wt_cache_get_stats(dir.File("file").c_str(), &c), WT_ERR_IO);
  EXPECT_EQ(wt_cache_get_stats(nullptr, &c), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_get_stats("", &c), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_get_stats(dir.str().c_str(), nullptr),
            WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_evict(nullptr, &c), WT_ERR_INVALID_ARGUMENT);
  EXPECT_EQ(wt_cache_clear("", &c), WT_ERR_INVALID_ARGUMENT);
}

TEST(CacheStoreTest, EvictsLeastRecentlyUsedFirst) {
  TempDir dir;
  dir.Make("old.wsc", 100, 300);
  dir.Make("mid.wsc", 100, 200);
  dir.Make("new.wsc", 100, 100);
  dir.Make("newest.wsc", 100, 0);
  CacheStats after;
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 250, 3600, &after), WT_OK);
  EXPECT_EQ(dir.List(), (Names{"new.wsc", "newest.wsc"}));
  EXPECT_EQ(after.file_count, 2u);
  EXPECT_EQ(after.total_bytes, 200u);
}

TEST(CacheStoreTest, WithinBudgetDeletesNothing) {
  TempDir dir;
  dir.Make("a.wsc", 100, 20);
  dir.Make("b.wsc", 100, 10);
  CacheStats after;
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 200, 3600, &after), WT_OK);
  EXPECT_EQ(dir.List(), (Names{"a.wsc", "b.wsc"}));
  EXPECT_EQ(after.total_bytes, 200u);
}

TEST(CacheStoreTest, BudgetZeroAndOversizedFile) {
  TempDir dir;
  dir.Make("a.wsc", 100, 20);
  dir.Make("b.wsc", 500, 10);
  CacheStats after;
  // A file larger than the budget goes too, even when it is the newest.
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 99, 3600, &after), WT_OK);
  EXPECT_TRUE(dir.List().empty());
  EXPECT_EQ(after.file_count, 0u);
  dir.Make("c.wsc", 1, 0);
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 0, 3600, nullptr), WT_OK);
  EXPECT_TRUE(dir.List().empty());
}

TEST(CacheStoreTest, EqualTimesEvictByName) {
  TempDir dir;
  for (const char* n : {"c.wsc", "a.wsc", "b.wsc"}) dir.Make(n, 10, 50);
  // Same mtime to the nanosecond: the name decides, so the result is stable.
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 10, 3600, nullptr), WT_OK);
  EXPECT_EQ(dir.List(), Names{"c.wsc"});
}

TEST(CacheStoreTest, StrayTemporariesGoAfterTheGracePeriod) {
  TempDir dir;
  dir.Make("a.wsc.1.0.tmp", 10, 7200);  // Left by a crash.
  dir.Make("b.wsc.2.0.tmp", 10, 5);     // A live writer's.
  dir.Make("a.wsc", 10, 0);
  dir.Make("other.tmp", 10, 7200);  // Not ours.
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), UINT64_MAX, 3600, nullptr), WT_OK);
  EXPECT_EQ(dir.List(), (Names{"b.wsc.2.0.tmp", "a.wsc", "other.tmp"}));
  // Temporaries don't count against the budget.
  CacheStats after;
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 10, 3600, &after), WT_OK);
  EXPECT_EQ(dir.List(), (Names{"b.wsc.2.0.tmp", "a.wsc", "other.tmp"}));
  EXPECT_EQ(after.total_bytes, 10u);
}

TEST(CacheStoreTest, OnlyOurFilesAreTouched) {
  TempDir dir;
  dir.Make("keep.txt", 1000, 9999);
  dir.Make("real.wsc", 1000, 9999);
  fs::create_directory(dir.File("sub"));
  dir.Make("sub/inner.wsc", 1000, 9999);
  fs::create_symlink(dir.File("real.wsc"), dir.File("link.wsc"));
  ASSERT_EQ(wt::CacheDirEvict(dir.str(), 0, 3600, nullptr), WT_OK);
  EXPECT_EQ(dir.List(), (Names{"keep.txt", "link.wsc", "sub"}));
  EXPECT_TRUE(fs::exists(dir.File("sub/inner.wsc")));
  fs::remove(dir.File("link.wsc"));
  dir.Make("real.wsc", 1000, 0);
  wt_cache_stats removed{};
  ASSERT_EQ(wt_cache_clear(dir.str().c_str(), &removed), WT_OK);
  EXPECT_EQ(removed.file_count, 1u);
  EXPECT_EQ(removed.total_bytes, 1000u);
  EXPECT_EQ(dir.List(), (Names{"keep.txt", "sub"}));
}

TEST(CacheStoreTest, ClearRemovesTemporariesToo) {
  TempDir dir;
  dir.Make("a.wsc", 10, 0);
  dir.Make("b.wsc", 20, 0);
  dir.Make("a.wsc.1.0.tmp", 10, 0);  // Even a fresh one.
  wt_cache_stats removed{};
  ASSERT_EQ(wt_cache_clear(dir.str().c_str(), &removed), WT_OK);
  EXPECT_EQ(removed.file_count, 2u);
  EXPECT_EQ(removed.total_bytes, 30u);
  EXPECT_TRUE(dir.List().empty());
}

TEST(CacheStoreTest, DefaultBudgetIs200MiB) {
  EXPECT_EQ(wt::kDefaultCacheBudget, uint64_t{200} << 20);
}

TEST(CacheStoreTest, CAbiEvictUsesTheBudget) {
  BudgetGuard guard;
  TempDir dir;
  dir.Make("a.wsc", 100, 30);
  dir.Make("b.wsc", 100, 20);
  dir.Make("c.wsc", 100, 10);
  wt_cache_set_budget(250);
  EXPECT_EQ(wt_cache_get_budget(), 250u);
  wt_cache_stats after{};
  ASSERT_EQ(wt_cache_evict(dir.str().c_str(), &after), WT_OK);
  EXPECT_EQ(after.file_count, 2u);
  EXPECT_EQ(after.total_bytes, 200u);
  EXPECT_EQ(dir.List(), (Names{"b.wsc", "c.wsc"}));
}

// --- The core enforcing the budget -------------------------------------------

constexpr const char* kClip = "vp8_alpha_256.webm";
constexpr uint32_t kSize = 64;

// Plays kClip to the end through wt_anim_create_cached, so a cache file is
// written (or read), and returns how many frames played.
size_t PlayCached(const std::string& path) {
  wt_test::Source source;
  EXPECT_EQ(wt_test::OpenSource(kClip, &source), WT_OK);
  wt_test::Anim anim;
  EXPECT_EQ(
      wt_anim_create_cached(source.p, kSize, kSize, path.c_str(), &anim.p),
      WT_OK);
  size_t frames = 0;
  wt_test::Bytes px;
  while (wt_test::Next(anim, &px) == WT_OK) ++frames;
  return frames;
}

uint64_t FileSize(const std::string& path) {
  std::error_code ec;
  const auto n = fs::file_size(path, ec);
  return ec ? 0 : n;
}

class CacheBudgetTest : public testing::Test {
 protected:
  void SetUp() override {
    // One clip's cache file: the unit the budgets below are made of.
    TempDir probe;
    ASSERT_GT(PlayCached(probe.File("p.wsc")), 0u);
    one_ = FileSize(probe.File("p.wsc"));
    ASSERT_GT(one_, 0u);
  }
  BudgetGuard guard_;
  uint64_t one_ = 0;
};

TEST_F(CacheBudgetTest, FinishedWritesEvictTheOldestFile) {
  TempDir dir;
  wt_cache_set_budget(one_ * 2 + one_ / 2);  // Room for two files.
  const std::string a = dir.File("a.wsc"), b = dir.File("b.wsc"),
                    c = dir.File("c.wsc");
  PlayCached(a);
  dir.Age("a.wsc", 300);
  PlayCached(b);
  dir.Age("b.wsc", 200);
  EXPECT_EQ(dir.List(), (Names{"a.wsc", "b.wsc"}));
  PlayCached(c);  // The third write pushes out the least recently used.
  EXPECT_EQ(dir.List(), (Names{"b.wsc", "c.wsc"}));
}

TEST_F(CacheBudgetTest, ACacheHitCountsAsUse) {
  TempDir dir;
  wt_cache_set_budget(one_ * 2 + one_ / 2);
  PlayCached(dir.File("a.wsc"));
  dir.Age("a.wsc", 300);
  PlayCached(dir.File("b.wsc"));
  dir.Age("b.wsc", 200);
  // Reading a.wsc makes it the most recently used.
  const auto before = fs::last_write_time(dir.File("a.wsc"));
  PlayCached(dir.File("a.wsc"));
  EXPECT_GT(fs::last_write_time(dir.File("a.wsc")), before);
  PlayCached(dir.File("c.wsc"));
  EXPECT_EQ(dir.List(), (Names{"a.wsc", "c.wsc"}));
}

TEST_F(CacheBudgetTest, ABudgetSmallerThanAFileKeepsNothingButStillPlays) {
  TempDir dir;
  wt_cache_set_budget(one_ - 1);
  EXPECT_GT(PlayCached(dir.File("a.wsc")), 0u);
  EXPECT_TRUE(dir.List().empty());
  // Every session decodes again, and still plays correctly.
  EXPECT_GT(PlayCached(dir.File("a.wsc")), 0u);
  EXPECT_TRUE(dir.List().empty());
}

TEST_F(CacheBudgetTest, UnlimitedBudgetKeepsEverything) {
  TempDir dir;
  wt_cache_set_budget(UINT64_MAX);
  for (const char* n : {"a.wsc", "b.wsc", "c.wsc"}) PlayCached(dir.File(n));
  EXPECT_EQ(dir.List().size(), 3u);
}

// Two animations write different files at once under a budget that holds
// one; whatever the interleaving, at most one file is left, no temporary
// survives, and both animations play every frame.
TEST_F(CacheBudgetTest, TwoAnimationsWritingConcurrently) {
  for (int round = 0; round < 8; ++round) {
    TempDir dir;
    wt_cache_set_budget(one_ + one_ / 2);
    std::atomic<size_t> frames[2] = {0, 0};
    std::thread t0([&] { frames[0] = PlayCached(dir.File("a.wsc")); });
    std::thread t1([&] { frames[1] = PlayCached(dir.File("b.wsc")); });
    t0.join();
    t1.join();
    EXPECT_GT(frames[0].load(), 0u);
    EXPECT_EQ(frames[0].load(), frames[1].load());
    const Names left = dir.List();
    EXPECT_LE(left.size(), 1u);
    for (const std::string& n : left) {
      EXPECT_EQ(n.find(".tmp"), std::string::npos) << n;
    }
    CacheStats stats;
    ASSERT_EQ(wt::CacheDirStats(dir.str(), &stats), WT_OK);
    EXPECT_LE(stats.total_bytes, wt_cache_get_budget());
  }
}

// Two animations write the same file at once: both play, and the one file
// that ends up there is valid.
TEST_F(CacheBudgetTest, TwoAnimationsWritingTheSameFile) {
  for (int round = 0; round < 8; ++round) {
    TempDir dir;
    wt_cache_set_budget(one_ * 4);
    const std::string path = dir.File("same.wsc");
    std::atomic<size_t> frames[2] = {0, 0};
    std::thread t0([&] { frames[0] = PlayCached(path); });
    std::thread t1([&] { frames[1] = PlayCached(path); });
    t0.join();
    t1.join();
    EXPECT_GT(frames[0].load(), 0u);
    EXPECT_EQ(frames[0].load(), frames[1].load());
    EXPECT_EQ(dir.List(), Names{"same.wsc"});
    EXPECT_EQ(FileSize(path), one_);
    EXPECT_EQ(PlayCached(path), frames[0].load());  // Reads it.
  }
}

// Concurrent evictions and writers on a shared directory never fail a frame or
// leave the directory over budget once everyone is done.
TEST_F(CacheBudgetTest, WritersAndExplicitEvictionsRace) {
  TempDir dir;
  wt_cache_set_budget(one_ * 2);
  std::atomic<bool> stop{false};
  std::thread evictor([&] {
    while (!stop) {
      wt_cache_stats after{};
      EXPECT_EQ(wt_cache_evict(dir.str().c_str(), &after), WT_OK);
    }
  });
  std::vector<std::thread> writers;
  std::atomic<size_t> played{0};
  for (int i = 0; i < 4; ++i) {
    writers.emplace_back([&, i] {
      for (int n = 0; n < 3; ++n) {
        const size_t frames =
            PlayCached(dir.File("w" + std::to_string(i * 3 + n) + ".wsc"));
        EXPECT_GT(frames, 0u);
        played += frames;
      }
    });
  }
  for (std::thread& t : writers) t.join();
  stop = true;
  evictor.join();
  EXPECT_GT(played.load(), 0u);
  wt_cache_stats after{};
  ASSERT_EQ(wt_cache_evict(dir.str().c_str(), &after), WT_OK);
  EXPECT_LE(after.total_bytes, one_ * 2);
}

}  // namespace
