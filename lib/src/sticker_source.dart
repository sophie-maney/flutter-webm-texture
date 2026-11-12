import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// Where a sticker's encoded bytes come from.
///
/// [StickerSource.file] and [StickerSource.bytes] are read directly.
/// [StickerSource.asset] loads from an `AssetBundle`, and
/// [StickerSource.network] goes through the app's [StickerFetcher].
///
/// Sources compare by value, so a widget rebuilt with an equal source keeps
/// its animation. Two [StickerSource.bytes] are equal when they share a `key`,
/// or, without one, the same `Uint8List` instance.
sealed class StickerSource {
  const StickerSource();

  /// A file on disk, read by the native core.
  ///
  /// The core maps the file while the sticker plays, so replace a file by
  /// writing a new one and renaming it over the old path, never by
  /// rewriting it in place: a file that shrinks under a mapping crashes the
  /// app (SIGBUS) on the next frame read past its new end.
  const factory StickerSource.file(String path) = FileStickerSource;

  /// Encoded bytes in memory. [key] identifies the content for caching; when
  /// null, the SHA-1 of the bytes is used.
  const factory StickerSource.bytes(Uint8List data, {String? key}) =
      BytesStickerSource;

  /// A sticker bundled as a Flutter asset, such as `assets/wave.webm`.
  ///
  /// Loaded from [bundle] (`rootBundle` when null). [package] names the
  /// package that declares the asset, as for `Image.asset`.
  const factory StickerSource.asset(
    String name, {
    AssetBundle? bundle,
    String? package,
  }) = AssetStickerSource;

  /// A sticker downloaded by the app's [StickerFetcher].
  ///
  /// [headers] are passed to the fetcher. [key] identifies the content; when
  /// null, the key follows from what the fetcher returns: the SHA-1 of
  /// bytes, or the path of a file.
  const factory StickerSource.network(
    String url, {
    Map<String, String>? headers,
    String? key,
  }) = NetworkStickerSource;
}

/// See [StickerSource.file].
final class FileStickerSource extends StickerSource {
  /// Creates a source for the file at [path].
  const FileStickerSource(this.path);

  /// Absolute path of the sticker file.
  final String path;

  @override
  bool operator ==(Object other) =>
      other is FileStickerSource && other.path == path;

  @override
  int get hashCode => Object.hash(FileStickerSource, path);
}

/// See [StickerSource.bytes].
final class BytesStickerSource extends StickerSource {
  /// Creates a source for [data].
  const BytesStickerSource(this.data, {this.key});

  /// The encoded sticker.
  final Uint8List data;

  /// Cache identity, or null to derive one from [data].
  final String? key;

  @override
  bool operator ==(Object other) =>
      other is BytesStickerSource &&
      other.key == key &&
      (key != null || identical(other.data, data));

  @override
  int get hashCode => key != null
      ? Object.hash(BytesStickerSource, key)
      : identityHashCode(data);
}

/// See [StickerSource.asset].
final class AssetStickerSource extends StickerSource {
  /// Creates a source for the asset [name].
  const AssetStickerSource(this.name, {this.bundle, this.package});

  /// The asset's path as declared in `pubspec.yaml`.
  final String name;

  /// Where to load from; `rootBundle` when null.
  final AssetBundle? bundle;

  /// The package that declares the asset, or null for the app's own.
  final String? package;

  /// The key to load from the bundle, with the `packages/` prefix if needed.
  String get assetKey => package == null ? name : 'packages/$package/$name';

  @override
  bool operator ==(Object other) =>
      other is AssetStickerSource &&
      other.name == name &&
      other.package == package &&
      identical(other.bundle, bundle);

  @override
  int get hashCode => Object.hash(AssetStickerSource, name, package, bundle);
}

/// See [StickerSource.network].
final class NetworkStickerSource extends StickerSource {
  /// Creates a source for [url].
  const NetworkStickerSource(this.url, {this.headers, this.key});

  /// The address to fetch.
  final String url;

  /// Request headers for the fetcher, such as authorization.
  final Map<String, String>? headers;

  /// Cache identity, or null to derive one from what the fetcher returns.
  final String? key;

  @override
  bool operator ==(Object other) =>
      other is NetworkStickerSource &&
      other.url == url &&
      other.key == key &&
      mapEquals(other.headers, headers);

  @override
  int get hashCode => Object.hash(NetworkStickerSource, url, key);
}

/// What a [StickerFetcher] found: a file the core can read, or the bytes.
sealed class StickerFetchResult {
  const StickerFetchResult();

  /// A sticker already on disk, at an absolute [path].
  ///
  /// Name files after their content (a hash, or a version in the path), as
  /// the path identifies the sticker in the animation cache. Never rewrite a
  /// file in place while it may be playing (see [StickerSource.file]): write
  /// a temporary file and rename it.
  const factory StickerFetchResult.file(String path) = FileFetchResult;

  /// A sticker in memory.
  const factory StickerFetchResult.bytes(Uint8List data) = BytesFetchResult;
}

/// See [StickerFetchResult.file].
final class FileFetchResult extends StickerFetchResult {
  /// Creates a result for the file at [path].
  const FileFetchResult(this.path);

  /// Absolute path of the sticker file.
  final String path;
}

/// See [StickerFetchResult.bytes].
final class BytesFetchResult extends StickerFetchResult {
  /// Creates a result for [data].
  const BytesFetchResult(this.data);

  /// The encoded sticker.
  final Uint8List data;
}

/// Downloads stickers for [StickerSource.network].
///
/// The plugin ships no HTTP client. An app implements this over its own
/// (with its own caching, auth and retries) and registers it with
/// `WebmStickers.fetcher`.
abstract interface class StickerFetcher {
  /// Fetches the sticker at [url], sending [headers] if given.
  ///
  /// Called on the UI isolate, at most once at a time per distinct source.
  /// Throw to report a failure; the exception reaches the widget's
  /// `errorBuilder`. Bytes over 2 MiB (the core's default input limit) are
  /// refused with a `WT_ERR_LIMIT` `StickerException` before they are hashed
  /// or cached, but a fetcher should refuse large downloads itself, before
  /// they are in memory.
  Future<StickerFetchResult> fetch(Uri url, {Map<String, String>? headers});
}

/// A [StickerSource] reduced to something the core can open, and the key that
/// names its animation and cache entry.
@immutable
class ResolvedSticker {
  /// Creates a resolved sticker; [source] must be a file or bytes source.
  const ResolvedSticker(this.key, this.source);

  /// Names the content: `key:` and the caller's key, `sha1:` and the hash of
  /// the bytes, or `file:` and a path.
  final String key;

  /// A [FileStickerSource] or [BytesStickerSource].
  final StickerSource source;
}
