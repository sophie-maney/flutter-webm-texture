import 'dart:typed_data';

/// Where a sticker's encoded bytes come from.
///
/// Only [StickerSource.file] and [StickerSource.bytes] exist so far. Asset and
/// network sources arrive with the widget.
sealed class StickerSource {
  const StickerSource();

  /// A file on disk, read by the native core.
  const factory StickerSource.file(String path) = FileStickerSource;

  /// Encoded bytes in memory. [key] identifies the content for caching; when
  /// null, a hash of the bytes is used.
  const factory StickerSource.bytes(Uint8List data, {String? key}) =
      BytesStickerSource;
}

/// See [StickerSource.file].
final class FileStickerSource extends StickerSource {
  /// Creates a source for the file at [path].
  const FileStickerSource(this.path);

  /// Absolute path of the sticker file.
  final String path;
}

/// See [StickerSource.bytes].
final class BytesStickerSource extends StickerSource {
  /// Creates a source for [data].
  const BytesStickerSource(this.data, {this.key});

  /// The encoded sticker.
  final Uint8List data;

  /// Cache identity, or null to derive one from [data].
  final String? key;
}
