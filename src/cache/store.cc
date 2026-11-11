#include "cache/store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <mutex>
#include <utility>
#include <vector>

#include "cache/sha256.h"

namespace wt {

namespace {

std::atomic<uint64_t> g_budget{kDefaultCacheBudget};
std::mutex g_evict_mutex;

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::char_traits<char>::length(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// "<name>.wsc", as the cache names its files.
bool IsCacheFile(const std::string& name) {
  return name.size() > 4 && EndsWith(name, ".wsc");
}

// "<name>.wsc.<pid>.<n>.tmp", as WscWriter names its temporaries.
bool IsTempFile(const std::string& name) {
  return EndsWith(name, ".tmp") && name.find(".wsc.") != std::string::npos;
}

struct Entry {
  std::string name;
  uint64_t size = 0;
  int64_t mtime_s = 0;
  int64_t mtime_ns = 0;
  bool temp = false;
};

int64_t MtimeSeconds(const struct stat& st) {
#ifdef __APPLE__
  return st.st_mtimespec.tv_sec;
#else
  return st.st_mtim.tv_sec;
#endif
}

int64_t MtimeNanos(const struct stat& st) {
#ifdef __APPLE__
  return st.st_mtimespec.tv_nsec;
#else
  return st.st_mtim.tv_nsec;
#endif
}

bool Unlink(int dir_fd, const std::string& name) {
  // Already gone (another process, a concurrent clear) is as good as deleted.
  return unlinkat(dir_fd, name.c_str(), 0) == 0 || errno == ENOENT;
}

// What Scan found. `overflow` counts files deleted for exceeding
// kMaxScannedFiles.
struct Scan {
  int dir_fd = -1;
  std::vector<Entry> entries;
  CacheStats overflow;
  ~Scan() {
    if (dir_fd >= 0) close(dir_fd);
  }
};

// Lists the regular cache files and temporaries directly inside dir.
// WT_OK with no entries when dir doesn't exist.
wt_status ScanDir(const std::string& dir, Scan* scan) {
  scan->dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (scan->dir_fd < 0) return errno == ENOENT ? WT_OK : WT_ERR_IO;
  // fdopendir takes over the descriptor, so give it a copy.
  const int copy = dup(scan->dir_fd);
  if (copy < 0) return WT_ERR_IO;
  DIR* d = fdopendir(copy);
  if (d == nullptr) {
    close(copy);
    return WT_ERR_IO;
  }
  while (const dirent* e = readdir(d)) {
    std::string name = e->d_name;
    const bool temp = IsTempFile(name);
    if (!temp && !IsCacheFile(name)) continue;
    struct stat st;
    if (fstatat(scan->dir_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(st.st_mode)) {
      continue;
    }
    if (scan->entries.size() >= kMaxScannedFiles) {
      if (Unlink(scan->dir_fd, name) && !temp) {
        ++scan->overflow.file_count;
        scan->overflow.total_bytes += static_cast<uint64_t>(st.st_size);
      }
      continue;
    }
    Entry entry;
    entry.name = std::move(name);
    entry.size = static_cast<uint64_t>(st.st_size);
    entry.mtime_s = MtimeSeconds(st);
    entry.mtime_ns = MtimeNanos(st);
    entry.temp = temp;
    scan->entries.push_back(std::move(entry));
  }
  closedir(d);
  return WT_OK;
}

void Add(CacheStats* stats, const Entry& e) {
  if (e.temp) return;
  ++stats->file_count;
  stats->total_bytes += e.size;
}

}  // namespace

uint64_t CacheBudget() { return g_budget.load(std::memory_order_relaxed); }

void SetCacheBudget(uint64_t bytes) {
  g_budget.store(bytes, std::memory_order_relaxed);
}

wt_status CacheDirStats(const std::string& dir, CacheStats* out) {
  *out = CacheStats{};
  Scan scan;
  const wt_status s = ScanDir(dir, &scan);
  if (s != WT_OK) return s;
  for (const Entry& e : scan.entries) Add(out, e);
  return WT_OK;
}

wt_status CacheDirEvict(const std::string& dir, uint64_t budget,
                        int64_t temp_grace_seconds, CacheStats* after) {
  if (after != nullptr) *after = CacheStats{};
  Scan scan;
  wt_status s = ScanDir(dir, &scan);
  if (s != WT_OK) return s;

  const int64_t now = static_cast<int64_t>(time(nullptr));
  CacheStats kept;
  std::vector<const Entry*> files;
  for (const Entry& e : scan.entries) {
    if (e.temp) {
      if (now - e.mtime_s >= temp_grace_seconds) Unlink(scan.dir_fd, e.name);
      continue;
    }
    files.push_back(&e);
    Add(&kept, e);
  }

  // Oldest first; the name breaks ties so the order is deterministic.
  std::sort(files.begin(), files.end(), [](const Entry* a, const Entry* b) {
    if (a->mtime_s != b->mtime_s) return a->mtime_s < b->mtime_s;
    if (a->mtime_ns != b->mtime_ns) return a->mtime_ns < b->mtime_ns;
    return a->name < b->name;
  });
  for (const Entry* e : files) {
    if (kept.total_bytes <= budget) break;
    if (!Unlink(scan.dir_fd, e->name)) continue;  // Stays, and counts.
    --kept.file_count;
    kept.total_bytes -= e->size;
  }
  if (after != nullptr) *after = kept;
  return WT_OK;
}

wt_status CacheDirClear(const std::string& dir, CacheStats* removed) {
  if (removed != nullptr) *removed = CacheStats{};
  Scan scan;
  const wt_status s = ScanDir(dir, &scan);
  if (s != WT_OK) return s;
  CacheStats gone = scan.overflow;
  for (const Entry& e : scan.entries) {
    if (Unlink(scan.dir_fd, e.name)) Add(&gone, e);
  }
  if (removed != nullptr) *removed = gone;
  return WT_OK;
}

void TouchCacheFile(const char* path) { utimensat(AT_FDCWD, path, nullptr, 0); }

wt_status CacheDirEvictLocked(const std::string& dir, CacheStats* after) {
  std::lock_guard<std::mutex> lock(g_evict_mutex);
  return CacheDirEvict(dir, CacheBudget(), kStrayTempAgeSeconds, after);
}

void EvictAfterWrite(const std::string& cache_path) {
  const size_t slash = cache_path.rfind('/');
  const std::string dir =
      slash == std::string::npos ? "." : cache_path.substr(0, slash + 1);
  if (CacheBudget() == UINT64_MAX) return;
  CacheDirEvictLocked(dir, nullptr);
}

wt_status CacheFilePath(const std::string& dir, const std::string& source_key,
                        uint32_t width, uint32_t height, std::string* out) {
  out->clear();
  if (dir.empty() || width == 0 || height == 0) return WT_ERR_INVALID_ARGUMENT;
  uint8_t digest[kSha256Size];
  Sha256(reinterpret_cast<const uint8_t*>(source_key.data()), source_key.size(),
         digest);
  static const char kHex[] = "0123456789abcdef";
  std::string path = dir;
  if (path.back() != '/') path += '/';
  for (size_t i = 0; i < 16; ++i) {
    path += kHex[digest[i] >> 4];
    path += kHex[digest[i] & 15];
  }
  path +=
      '_' + std::to_string(width) + 'x' + std::to_string(height) + "_v1.wsc";
  *out = std::move(path);
  return WT_OK;
}

}  // namespace wt
