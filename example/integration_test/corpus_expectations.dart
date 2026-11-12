// What probing each file in test_assets/manifest.json must return. Shared by
// the device integration test and the host unit test (test/probe_test.dart).
import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/webm_texture.dart';

/// Checks one manifest [entry] against [probe], which probes that file.
Future<void> expectProbeMatchesManifest(
  Map<String, dynamic> entry,
  Future<StickerInfo> Function() probe,
) async {
  final name = entry['name'] as String;
  final expect_ = entry['expect'] as String;
  final kind = entry['kind'] as String;

  Matcher throwsStatus(String status) =>
      throwsA(isA<StickerException>().having((e) => e.name, 'name', status));

  if (expect_ == 'limit_exceeded') {
    await expectLater(probe(), throwsStatus('WT_ERR_LIMIT'), reason: name);
    return;
  }
  if (expect_ == 'invalid') {
    await expectLater(
      probe(),
      throwsStatus('WT_ERR_INVALID_DATA'),
      reason: name,
    );
    return;
  }
  if (kind == 'gif') {
    // GIF sniffs but doesn't probe until T5.2.
    await expectLater(
      probe(),
      throwsStatus('WT_ERR_UNSUPPORTED'),
      reason: name,
    );
    return;
  }

  final info = await probe();
  final durations = (entry['durations_ms'] as List).cast<int>();
  final totalMs = durations.fold<int>(0, (a, b) => a + b);
  final frames = entry['frame_count'] as int;
  expect(
    info.format,
    kind == 'webp' ? StickerFormat.webp : StickerFormat.webm,
    reason: name,
  );
  expect(info.codec.name, entry['codec'], reason: '$name codec');
  expect(info.width, entry['width'], reason: '$name width');
  expect(info.height, entry['height'], reason: '$name height');
  expect(info.frameCount, frames, reason: '$name frames');
  expect(info.duration.inMilliseconds, totalMs, reason: '$name duration');
  expect(info.fps, closeTo(frames * 1000 / totalMs, 1e-6), reason: '$name fps');
  expect(info.hasAlpha, entry['alpha'], reason: '$name alpha');
  expect(info.hasAudio, entry['has_audio'], reason: '$name audio');
  expect(info.truncated, expect_ == 'truncated', reason: '$name truncated');
}
