import 'dart:collection';

import 'package:flutter/services.dart';

import 'gen/wt_bindings.g.dart' show WT_ERR_LIMIT;
import 'sha1.dart';
import 'sticker_info.dart';
import 'sticker_source.dart';

/// The core's default `max_bytes` (`wt_limits_init_default`), which playback
/// always uses. Fetched bytes over it are refused before they are hashed or
/// cached, since the core would refuse them anyway.
const int maxFetchedStickerBytes = 2 << 20;

/// Turns any [StickerSource] into a [ResolvedSticker]: loads assets, calls
/// the [StickerFetcher] for network sources, and names the content.
///
/// Used from the UI isolate only.
class StickerResolver {
  /// Creates a resolver that keeps up to [cacheSize] asset and network
  /// results, so that widgets showing the same one load it once.
  StickerResolver({this.fetcher, this.cacheSize = 32});

  /// Handles [StickerSource.network]; without one, those sources fail.
  StickerFetcher? fetcher;

  /// How many asset and network results are kept (their bytes included).
  final int cacheSize;

  // Insertion order is recency order: a hit is moved to the end.
  final LinkedHashMap<StickerSource, Future<ResolvedSticker>> _cache =
      LinkedHashMap();

  static final Expando<String> _hashes = Expando('webm_texture content hash');

  /// Resolves [source]. Concurrent calls for an equal source share one load.
  /// Failures aren't cached, so a retry fetches again.
  Future<ResolvedSticker> resolve(StickerSource source) {
    // A NUL would end the key or the path early in the native glue (iOS
    // passes them as C strings), so two different keys could share a cache
    // file.
    final name = switch (source) {
      FileStickerSource(:final path) => path,
      BytesStickerSource(:final key) || NetworkStickerSource(:final key) => key,
      AssetStickerSource() => null,
    };
    if (name != null && name.contains('\u0000')) {
      return Future.error(
        ArgumentError.value(name, 'source', 'must not contain NUL'),
      );
    }
    switch (source) {
      case FileStickerSource(:final path):
        return Future.value(ResolvedSticker('file:$path', source));
      case BytesStickerSource(:final data, :final key):
        return Future.value(
          ResolvedSticker(key != null ? 'key:$key' : _hashOf(data), source),
        );
      case AssetStickerSource() || NetworkStickerSource():
        final hit = _cache.remove(source);
        if (hit != null) return _cache[source] = hit;
        final load = _load(source);
        _cache[source] = load;
        if (_cache.length > cacheSize) _cache.remove(_cache.keys.first);
        load.then<void>(
          (_) {},
          onError: (Object _) {
            if (identical(_cache[source], load)) _cache.remove(source);
          },
        );
        return load;
    }
  }

  /// Forgets cached asset and network results.
  void clear() => _cache.clear();

  Future<ResolvedSticker> _load(StickerSource source) async {
    switch (source) {
      case AssetStickerSource(:final bundle, :final assetKey):
        final bytes = _view(await (bundle ?? rootBundle).load(assetKey));
        return _fromBytes(bytes, null);
      case NetworkStickerSource(:final url, :final headers, :final key):
        final fetcher = this.fetcher;
        if (fetcher == null) {
          throw StateError(
            'StickerSource.network needs a StickerFetcher: set '
            'WebmStickers.fetcher before showing "$url".',
          );
        }
        final result = await fetcher.fetch(Uri.parse(url), headers: headers);
        return switch (result) {
          FileFetchResult(:final path) when path.contains('\u0000') =>
            throw ArgumentError.value(path, 'path', 'must not contain NUL'),
          FileFetchResult(:final path) => ResolvedSticker(
            key != null ? 'key:$key' : 'file:$path',
            StickerSource.file(path),
          ),
          BytesFetchResult(:final data)
              when data.length > maxFetchedStickerBytes =>
            throw StickerException(
              WT_ERR_LIMIT,
              'WT_ERR_LIMIT',
              '${data.length} bytes, over $maxFetchedStickerBytes',
            ),
          BytesFetchResult(:final data) => _fromBytes(data, key),
        };
      case FileStickerSource() || BytesStickerSource():
        throw ArgumentError.value(source, 'source', 'needs no loading');
    }
  }

  ResolvedSticker _fromBytes(Uint8List data, String? key) => ResolvedSticker(
    key != null ? 'key:$key' : _hashOf(data),
    StickerSource.bytes(data),
  );

  static Uint8List _view(ByteData d) =>
      d.buffer.asUint8List(d.offsetInBytes, d.lengthInBytes);

  /// `sha1:<hex>`, computed once per buffer instance.
  static String _hashOf(Uint8List data) =>
      _hashes[data] ??= 'sha1:${sha1Hex(data)}';
}
