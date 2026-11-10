import 'dart:typed_data';

/// SHA-1 of [data] as 40 lowercase hex digits.
///
/// Names content for caching and deduplication, not for security: a collision
/// would need a crafted pair of stickers, and the worst outcome is one
/// sticker showing in place of another. Pure Dart so that no dependency is
/// needed; about 10 ms per MB on a phone, so run it once per buffer.
String sha1Hex(Uint8List data) {
  var h0 = 0x67452301;
  var h1 = 0xEFCDAB89;
  var h2 = 0x98BADCFE;
  var h3 = 0x10325476;
  var h4 = 0xC3D2E1F0;

  // Message, 0x80, zero padding to 56 mod 64, then the bit length as a
  // 64-bit big-endian number. Only the tail is copied.
  final fullBlocks = data.length ~/ 64;
  final tailLength = data.length - fullBlocks * 64;
  final tail = Uint8List(tailLength < 56 ? 64 : 128)
    ..setRange(0, tailLength, data, fullBlocks * 64);
  tail[tailLength] = 0x80;
  final bitLength = data.length * 8;
  final tailView = ByteData.sublistView(tail);
  tailView.setUint32(tail.length - 8, bitLength ~/ 0x100000000);
  tailView.setUint32(tail.length - 4, bitLength & 0xFFFFFFFF);

  final w = Uint32List(80);
  void block(ByteData view, int offset) {
    for (var i = 0; i < 16; i++) {
      w[i] = view.getUint32(offset + i * 4);
    }
    for (var i = 16; i < 80; i++) {
      final x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
      w[i] = ((x << 1) | (x >>> 31)) & 0xFFFFFFFF;
    }
    var a = h0, b = h1, c = h2, d = h3, e = h4;
    for (var i = 0; i < 80; i++) {
      final int f, k;
      if (i < 20) {
        f = (b & c) | (~b & 0xFFFFFFFF & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      final t =
          ((((a << 5) | (a >>> 27)) & 0xFFFFFFFF) + f + e + k + w[i]) &
          0xFFFFFFFF;
      e = d;
      d = c;
      c = ((b << 30) | (b >>> 2)) & 0xFFFFFFFF;
      b = a;
      a = t;
    }
    h0 = (h0 + a) & 0xFFFFFFFF;
    h1 = (h1 + b) & 0xFFFFFFFF;
    h2 = (h2 + c) & 0xFFFFFFFF;
    h3 = (h3 + d) & 0xFFFFFFFF;
    h4 = (h4 + e) & 0xFFFFFFFF;
  }

  final view = ByteData.sublistView(data);
  for (var i = 0; i < fullBlocks; i++) {
    block(view, i * 64);
  }
  for (var offset = 0; offset < tail.length; offset += 64) {
    block(tailView, offset);
  }

  String hex(int v) => v.toRadixString(16).padLeft(8, '0');
  return hex(h0) + hex(h1) + hex(h2) + hex(h3) + hex(h4);
}
