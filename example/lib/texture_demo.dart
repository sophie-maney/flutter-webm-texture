// A bare page over the platform texture glue (T2.2/T2.3): it drives the Pigeon
// host API directly because the registry and the `WebmSticker` widget (T2.4,
// T2.5) don't exist yet. T2.7 replaces it with the real example pages.
// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:webm_texture/src/gen/messages.g.dart';

/// Corpus files shown on the page, with the bucket each one is created at
/// (0 x 0 means the source size).
const _stickers = [
  ('vp9_alpha_512.webm', 256, 256),
  ('vp8_alpha_256.webm', 256, 256),
  ('vp9_alpha_vfr.webm', 256, 256),
  ('vp9_opaque_256.webm', 128, 128),
  ('vp9_odd_301x199.webm', 0, 0),
  ('vp9_single_frame.webm', 256, 256),
];

class TextureDemoPage extends StatefulWidget {
  const TextureDemoPage({super.key});

  @override
  State<TextureDemoPage> createState() => _TextureDemoPageState();
}

class _Live {
  _Live(this.name, this.handle, this.width, this.height);
  final String name;
  final AnimationHandleMessage handle;
  final int width;
  final int height;
}

class _TextureDemoPageState extends State<TextureDemoPage> {
  final _api = WebmTextureHostApi();
  final _live = <_Live>[];
  final _log = <String>[];
  StreamSubscription<AnimationErrorMessage>? _errors;
  bool _playing = true;
  bool _busy = false;

  @override
  void initState() {
    super.initState();
    _errors = streamErrors().listen(
      (e) => _say('anim ${e.animId}: ${e.code} ${e.message}'),
    );
    unawaited(_createAll());
  }

  @override
  void dispose() {
    _errors?.cancel();
    for (final l in _live) {
      _api.dispose(l.handle.animId);
    }
    super.dispose();
  }

  void _say(String line) {
    if (!mounted) return;
    setState(() => _log.insert(0, line));
  }

  Future<AnimationSourceMessage> _source(
    String name, {
    bool asFile = false,
  }) async {
    final bytes = (await rootBundle.load('test_assets/$name')).buffer
        .asUint8List();
    if (!asFile) return AnimationSourceMessage(bytes: bytes);
    final file = File('${Directory.systemTemp.path}/wt_demo_$name');
    await file.writeAsBytes(bytes, flush: true);
    return AnimationSourceMessage(path: file.path);
  }

  Future<void> _createAll() async {
    for (final (i, (name, w, h)) in _stickers.indexed) {
      try {
        // Alternate bytes and paths so both source kinds are exercised.
        final src = await _source(name, asFile: i.isOdd);
        final handle = await _api.createAnimation(name, src, w, h);
        await _api.setActive(handle.animId, _playing);
        if (!mounted) {
          await _api.dispose(handle.animId);
          return;
        }
        setState(() => _live.add(_Live(name, handle, w, h)));
      } on PlatformException catch (e) {
        _say('$name: ${e.code}');
      }
    }
    // Rejected input must fail with the core's status name.
    for (final name in ['vp9_2048.webm', 'garbage.bin', 'truncated.webm']) {
      try {
        final handle = await _api.createAnimation(
          name,
          await _source(name),
          256,
          256,
        );
        _say('$name: created (texture ${handle.textureId})');
        await _api.dispose(handle.animId);
      } on PlatformException catch (e) {
        _say('$name: ${e.code}');
      }
    }
  }

  Future<void> _togglePlay() async {
    _playing = !_playing;
    for (final l in _live) {
      await _api.setActive(l.handle.animId, _playing);
    }
    setState(() {});
  }

  /// Creates, plays one frame's worth and disposes an animation [n] times,
  /// for leak checks (Instruments / `leaks`, or `dumpsys meminfo`).
  Future<void> _cycle(int n) async {
    setState(() => _busy = true);
    final sw = Stopwatch()..start();
    final src = await _source('vp9_alpha_512.webm');
    for (var i = 0; i < n; i++) {
      final h = await _api.createAnimation('cycle', src, 256, 256);
      await _api.setActive(h.animId, true);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      await _api.dispose(h.animId);
      await _api.dispose(h.animId); // idempotent
    }
    _say('cycled $n in ${sw.elapsedMilliseconds} ms');
    setState(() => _busy = false);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Textures'),
        actions: [
          IconButton(
            key: const Key('play'),
            tooltip: _playing ? 'Pause' : 'Play',
            icon: Icon(_playing ? Icons.pause : Icons.play_arrow),
            onPressed: _togglePlay,
          ),
          TextButton(
            key: const Key('cycle'),
            onPressed: _busy ? null : () => _cycle(100),
            child: Text(_busy ? 'Cycling…' : 'Cycle ×100'),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              for (final l in _live)
                _Tile(label: l.name, textureId: l.handle.textureId),
            ],
          ),
          const SizedBox(height: 12),
          Text('${_live.length} live', key: const Key('liveCount')),
          for (final line in _log)
            Text(line, style: const TextStyle(fontSize: 12)),
        ],
      ),
    );
  }
}

class _Tile extends StatelessWidget {
  const _Tile({required this.label, required this.textureId});
  final String label;
  final int textureId;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          width: 110,
          height: 110,
          child: CustomPaint(
            painter: _Checker(),
            child: Texture(
              textureId: textureId,
              filterQuality: FilterQuality.medium,
            ),
          ),
        ),
        SizedBox(
          width: 110,
          child: Text(
            label,
            style: const TextStyle(fontSize: 10),
            overflow: TextOverflow.ellipsis,
          ),
        ),
      ],
    );
  }
}

/// A checkerboard behind each texture, so transparency is visible.
class _Checker extends CustomPainter {
  @override
  void paint(Canvas canvas, Size size) {
    const cell = 10.0;
    final dark = Paint()..color = const Color(0xFF9E9E9E);
    canvas.drawRect(
      Offset.zero & size,
      Paint()..color = const Color(0xFFFFFFFF),
    );
    for (var y = 0; y * cell < size.height; y++) {
      for (var x = 0; x * cell < size.width; x++) {
        if ((x + y).isOdd) {
          canvas.drawRect(Rect.fromLTWH(x * cell, y * cell, cell, cell), dark);
        }
      }
    }
  }

  @override
  bool shouldRepaint(_Checker oldDelegate) => false;
}
