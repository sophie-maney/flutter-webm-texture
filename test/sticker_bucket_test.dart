import 'dart:ui' show Size;

import 'package:flutter_test/flutter_test.dart';
import 'package:webm_texture/src/sticker_bucket.dart';

void main() {
  group('roundUp', () {
    test('snaps up to the ladder', () {
      expect(StickerBuckets.roundUp(1), 64);
      expect(StickerBuckets.roundUp(64), 64);
      expect(StickerBuckets.roundUp(64.1), 96);
      expect(StickerBuckets.roundUp(130), 192);
      expect(StickerBuckets.roundUp(384), 384);
      expect(StickerBuckets.roundUp(385), 512);
    });

    test('clamps above the last step', () {
      expect(StickerBuckets.roundUp(513), 512);
      expect(StickerBuckets.roundUp(4000), 512);
    });

    test('rejects non-positive and non-finite input', () {
      expect(() => StickerBuckets.roundUp(0), throwsArgumentError);
      expect(() => StickerBuckets.roundUp(-1), throwsArgumentError);
      expect(() => StickerBuckets.roundUp(double.nan), throwsArgumentError);
      expect(
        () => StickerBuckets.roundUp(double.infinity),
        throwsArgumentError,
      );
    });
  });

  group('forSize', () {
    test('multiplies by the device pixel ratio', () {
      // 80 dp x 3.0 = 240 px -> 256.
      expect(
        StickerBuckets.forSize(const Size(80, 80), 3),
        const StickerBucket(256, 256),
      );
      // The same widget at 2.0 lands in a smaller bucket: 160 px -> 192.
      expect(
        StickerBuckets.forSize(const Size(80, 80), 2),
        const StickerBucket(192, 192),
      );
    });

    test('keeps the box aspect ratio when no aspect is given', () {
      // 100 x 50 dp at 2.0 = 200 x 100 px; longer side 256, scale 1.28.
      expect(
        StickerBuckets.forSize(const Size(100, 50), 2),
        const StickerBucket(256, 128),
      );
      expect(
        StickerBuckets.forSize(const Size(50, 100), 2),
        const StickerBucket(128, 256),
      );
    });

    test('fits the source aspect inside the box', () {
      // A square sticker in a 200 x 100 dp box shows at 100 x 100 dp.
      expect(
        StickerBuckets.forSize(const Size(200, 100), 1, aspect: 1),
        const StickerBucket(128, 128),
      );
      // A 2:1 sticker in a square box fills the width only.
      expect(
        StickerBuckets.forSize(const Size(100, 100), 1, aspect: 2),
        const StickerBucket(128, 64),
      );
    });

    test('never returns a zero side', () {
      expect(
        StickerBuckets.forSize(const Size(1000, 1), 1),
        const StickerBucket(512, 1),
      );
    });

    test('same-ladder sizes share a bucket', () {
      expect(
        StickerBuckets.forSize(const Size(70, 70), 1),
        StickerBuckets.forSize(const Size(90, 90), 1),
      );
    });

    test('rejects invalid input', () {
      expect(
        () => StickerBuckets.forSize(const Size(0, 10), 1),
        throwsArgumentError,
      );
      expect(
        () => StickerBuckets.forSize(const Size(10, 10), 0),
        throwsArgumentError,
      );
      expect(
        () => StickerBuckets.forSize(const Size(10, 10), 1, aspect: 0),
        throwsArgumentError,
      );
    });
  });
}
