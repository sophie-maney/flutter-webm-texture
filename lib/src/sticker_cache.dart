import 'dart:ffi';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';

import 'gen/messages.g.dart';
import 'gen/wt_bindings.g.dart';
import 'native_library.dart';
import 'sticker_config.dart';
import 'sticker_info.dart';

/// How much the frame cache holds, from [WebmStickers.cacheStats].
@immutable
class CacheStats {
  /// Creates statistics.
  const CacheStats({required this.fileCount, required this.totalBytes});

  /// Number of cached animations (one file per animation and size).
  final int fileCount;

  /// Their combined size, in bytes.
  final int totalBytes;

  @override
  bool operator ==(Object other) =>
      other is CacheStats &&
      other.fileCount == fileCount &&
      other.totalBytes == totalBytes;

  @override
  int get hashCode => Object.hash(fileCount, totalBytes);

  @override
  String toString() => 'CacheStats($fileCount files, $totalBytes bytes)';
}

/// The cache directory functions of the native core, on background isolates.
/// A seam for tests that have no native library.
abstract interface class CacheNative {
  /// Sets the process-wide disk budget, then evicts [directory] down to it.
  Future<void> applyBudget(String directory, int budgetBytes);

  /// Counts the cache files in [directory].
  Future<CacheStats> stats(String directory);

  /// Deletes every cache file in [directory].
  Future<void> clear(String directory);
}

/// [CacheNative] over `dart:ffi` (`wt_cache_*` in wt_api.h).
class FfiCacheNative implements CacheNative {
  /// Creates the FFI implementation.
  const FfiCacheNative();

  @override
  Future<void> applyBudget(String directory, int budgetBytes) {
    // StickerConfig asserts this, but release builds drop asserts, and FFI
    // would store -1 as UINT64_MAX: a cache that is never evicted.
    if (budgetBytes < 0) {
      return Future.error(
        RangeError.range(budgetBytes, 0, null, 'diskBudgetBytes'),
      );
    }
    return Isolate.run(() {
      final b = loadBindings();
      b.wt_cache_set_budget(budgetBytes);
      // Best effort, like every cache problem: a directory that can't be
      // read now has nothing to evict, and stats() reports why.
      try {
        _withDir(b, directory, (dir, out) => b.wt_cache_evict(dir, out));
      } on StickerException {
        // Ignored.
      }
    });
  }

  @override
  Future<CacheStats> stats(String directory) => Isolate.run(() {
    final b = loadBindings();
    return _withDir(b, directory, (dir, out) => b.wt_cache_get_stats(dir, out));
  });

  @override
  Future<void> clear(String directory) => Isolate.run(() {
    final b = loadBindings();
    _withDir(b, directory, (dir, out) => b.wt_cache_clear(dir, out));
  });

  static CacheStats _withDir(
    WtBindings b,
    String directory,
    int Function(Pointer<Char> dir, Pointer<wt_cache_stats> out) call,
  ) {
    final arena = Arena();
    try {
      final out = arena<wt_cache_stats>();
      final status = call(directory.toNativeUtf8(allocator: arena).cast(), out);
      if (status < 0) throw StickerException(status, statusName(b, status));
      return CacheStats(
        fileCount: out.ref.file_count,
        totalBytes: out.ref.total_bytes,
      );
    } finally {
      arena.releaseAll();
    }
  }
}

/// Process-wide control of the frame cache: where it lives, how big it may
/// grow, and what it holds. Used from the UI isolate.
class StickerCache {
  /// Creates the cache control over [host] (the real platform API when null)
  /// and [native] (FFI when null).
  StickerCache({WebmTextureHostApi? host, CacheNative? native})
    : _host = host ?? WebmTextureHostApi(),
      _native = native ?? const FfiCacheNative();

  final WebmTextureHostApi _host;
  final CacheNative _native;

  /// Applies [config]: the platform starts caching in the directory (the
  /// default one when [StickerConfig.cacheDirectory] is null), and the budget
  /// is enforced right away.
  ///
  /// Animations created before this call keep the files they use.
  Future<void> configure(StickerConfig config) async {
    final directory = await _host.configureCache(config.cacheDirectory);
    await _native.applyBudget(directory, config.diskBudgetBytes);
  }

  /// What the cache holds now.
  Future<CacheStats> stats() async =>
      _native.stats(await _host.configureCache(null));

  /// Deletes every cached frame file. Playing animations carry on; they
  /// decode again (and write a new file) when their file is gone.
  Future<void> clear() async => _native.clear(await _host.configureCache(null));
}
