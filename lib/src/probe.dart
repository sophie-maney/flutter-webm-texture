import 'dart:ffi';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'gen/wt_bindings.g.dart';
import 'native_library.dart';
import 'sticker_info.dart';
import 'sticker_source.dart';

/// Reads [source]'s container headers and frame index on a background isolate.
///
/// The core enforces its limits while probing, so a sticker that probes OK is
/// within them. Throws [StickerException] for anything the core won't play,
/// and [RangeError] for a [limits] field the native `wt_limits` can't hold.
Future<StickerInfo> probeSticker(
  StickerSource source, {
  StickerLimits? limits,
}) async {
  // Before the isolate, so a bad field fails here with a plain RangeError.
  if (limits != null) _checkLimits(limits);
  // Isolate.run copies what the closure captures; both are small (a path, or
  // at most max_bytes of sticker data).
  return Isolate.run(() => _probeSync(source, limits));
}

const int _maxUint32 = 0xFFFFFFFF;

/// FFI stores a Dart int into a `uint32_t`/`uint64_t` field modulo 2^32/2^64,
/// so -1 would become the largest value: a "tighter" override that switches
/// the limit off. Every field must be in 1..the field's maximum.
void _checkLimits(StickerLimits limits) {
  void check(int? value, int max, String name) {
    if (value != null && (value < 1 || value > max)) {
      throw RangeError.range(value, 1, max, name);
    }
  }

  check(limits.maxDimension, _maxUint32, 'maxDimension');
  check(limits.maxDuration?.inMilliseconds, _maxUint32, 'maxDuration');
  // A Dart int is at most 2^63 - 1, which a uint64_t holds.
  check(limits.maxBytes, 0x7FFFFFFFFFFFFFFF, 'maxBytes');
  check(limits.maxFrames, _maxUint32, 'maxFrames');
}

StickerInfo _probeSync(StickerSource source, StickerLimits? limits) {
  final b = loadBindings();
  final arena = Arena();
  try {
    final nativeLimits = _toNativeLimits(b, arena, limits);
    final outSource = arena<Pointer<wt_source>>();
    final int created;
    switch (source) {
      case FileStickerSource(:final path):
        created = b.wt_source_create_file(
          path.toNativeUtf8(allocator: arena).cast<Char>(),
          nativeLimits,
          outSource,
        );
      case BytesStickerSource(:final data):
        final buffer = arena<Uint8>(data.isEmpty ? 1 : data.length);
        buffer.asTypedList(data.length).setAll(0, data);
        created = b.wt_source_create_memory(
          buffer,
          data.length,
          nativeLimits,
          outSource,
        );
      case AssetStickerSource() || NetworkStickerSource():
        throw ArgumentError.value(source, 'source', 'must be resolved first');
    }
    _check(b, created);

    final handle = outSource.value;
    try {
      final info = arena<wt_info>();
      _check(b, b.wt_probe(handle, info));
      return _fromNative(info.ref);
    } finally {
      b.wt_source_destroy(handle);
    }
  } finally {
    arena.releaseAll();
  }
}

Pointer<wt_limits> _toNativeLimits(
  WtBindings b,
  Allocator allocator,
  StickerLimits? limits,
) {
  if (limits == null) return nullptr;
  final out = allocator<wt_limits>();
  b.wt_limits_init_default(out);
  if (limits.maxDimension != null) out.ref.max_dimension = limits.maxDimension!;
  if (limits.maxDuration != null) {
    out.ref.max_duration_ms = limits.maxDuration!.inMilliseconds;
  }
  if (limits.maxBytes != null) out.ref.max_bytes = limits.maxBytes!;
  if (limits.maxFrames != null) out.ref.max_frames = limits.maxFrames!;
  return out;
}

void _check(WtBindings b, int status) {
  if (status < 0) throw StickerException(status, statusName(b, status));
}

StickerInfo _fromNative(wt_info i) => StickerInfo(
  format: _enumOrUnknown(StickerFormat.values, i.format),
  codec: _enumOrUnknown(StickerCodec.values, i.codec),
  width: i.width,
  height: i.height,
  frameCount: i.frame_count,
  duration: Duration(milliseconds: i.duration_ms),
  fps: i.fps,
  hasAlpha: i.has_alpha,
  hasAudio: i.has_audio,
  truncated: i.truncated,
);

/// The core's format and codec codes are 0 = unknown, then 1, 2, ... in the
/// same order as the Dart enums; a code this build doesn't know maps to
/// `unknown` (index 0).
T _enumOrUnknown<T>(List<T> values, int code) =>
    code >= 0 && code < values.length ? values[code] : values[0];
