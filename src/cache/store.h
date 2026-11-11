// The cache directory (T3.2): which .wsc files are there, how much space they
// use, and which to delete when the budget is exceeded.
//
// docs/cache-format.md describes one file; this is the policy around many:
//
// - Only files this module recognises are ever touched, and only directly in
//   the directory (no recursion, no symlinks): "*.wsc" cache files and
//   "*.wsc.*.tmp" writer temporaries. Pointing the cache at a directory with
//   other content is therefore safe.
// - LRU runs on mtime. Access times are unreliable (noatime and relatime
//   mounts), so a cache hit "touches" the file (TouchCacheFile) and a freshly
//   written file starts with the time of its rename.
// - Eviction deletes the least recently used files until the total is within
//   the budget. It also deletes writer temporaries that haven't been written
//   to for a grace period: they are left by a crash, while a live writer
//   keeps modifying its file. A writer that loses its temporary file anyway
//   (paused for longer than the grace period, or a clearCache) fails its
//   Finish and its animation just goes on decoding.
// - Unlinking a file is safe while animations have it mapped: they keep
//   reading it, and the space comes back when they let go.
// - The budget is process-wide (SetCacheBudget). Every finished write evicts
//   its directory down to it, serialised by a mutex, so concurrent writers
//   can't overshoot together. Several processes are tolerated: every
//   deletion shrugs off ENOENT.
//
// Thread: background thread (directory scans and unlinks). Everything here is
// safe to call concurrently.
#ifndef WT_CACHE_STORE_H_
#define WT_CACHE_STORE_H_

#include <cstdint>
#include <string>

#include "api/wt_api.h"

namespace wt {

// 200 MiB, PLAN.md "Frame cache".
constexpr uint64_t kDefaultCacheBudget = uint64_t{200} << 20;

// How long a writer temporary may sit untouched before eviction deletes it.
constexpr int64_t kStrayTempAgeSeconds = 3600;

// At most this many cache files are listed per scan; any beyond it are
// deleted outright, so a runaway directory can't make a scan allocate without
// bound.
constexpr size_t kMaxScannedFiles = 65536;

struct CacheStats {
  uint64_t file_count = 0;  // *.wsc files.
  uint64_t total_bytes = 0;
};

// The process-wide budget used after each cache write. Default
// kDefaultCacheBudget; UINT64_MAX means unlimited.
uint64_t CacheBudget();
void SetCacheBudget(uint64_t bytes);

// Counts the cache files in dir. A directory that doesn't exist holds
// nothing (WT_OK, zeros); WT_ERR_IO for any other failure to read it.
wt_status CacheDirStats(const std::string& dir, CacheStats* out);

// Deletes least-recently-used cache files until at most budget bytes remain,
// and stray temporaries older than temp_grace_seconds. *after (may be null)
// receives what is left. Files that can't be deleted count as left.
wt_status CacheDirEvict(const std::string& dir, uint64_t budget,
                        int64_t temp_grace_seconds, CacheStats* after);

// CacheDirEvict down to CacheBudget() with the default temporary grace, under
// the process-wide eviction lock.
wt_status CacheDirEvictLocked(const std::string& dir, CacheStats* after);

// Deletes every cache file and every writer temporary in dir. *removed (may
// be null) receives what was deleted.
wt_status CacheDirClear(const std::string& dir, CacheStats* removed);

// Marks path as used just now, for LRU. Failure is ignored.
void TouchCacheFile(const char* path);

// Evicts the directory holding cache_path (see CacheDirEvictLocked). Called
// after each finished write; does nothing when the budget is unlimited.
void EvictAfterWrite(const std::string& cache_path);

// "<dir>/<hash>_<width>x<height>_v1.wsc": <hash> is 32 hex digits taken from
// the SHA-256 of source_key, so any key becomes a safe file name. dir must be
// non-empty. WT_ERR_INVALID_ARGUMENT for an empty dir or a zero size.
wt_status CacheFilePath(const std::string& dir, const std::string& source_key,
                        uint32_t width, uint32_t height, std::string* out);

}  // namespace wt

#endif  // WT_CACHE_STORE_H_
