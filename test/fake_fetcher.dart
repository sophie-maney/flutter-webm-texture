import 'dart:async';
import 'dart:typed_data';

import 'package:webm_texture/webm_texture.dart';

/// Records fetches and returns a canned result instead of using a network.
class FakeFetcher implements StickerFetcher {
  final List<(Uri, Map<String, String>?)> calls = [];
  Completer<void>? gate;
  Object? failWith;
  StickerFetchResult result = StickerFetchResult.bytes(Uint8List.fromList([1]));

  @override
  Future<StickerFetchResult> fetch(
    Uri url, {
    Map<String, String>? headers,
  }) async {
    calls.add((url, headers));
    await gate?.future;
    if (failWith != null) throw failWith!;
    return result;
  }
}
