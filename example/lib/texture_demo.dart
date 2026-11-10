// A bare page over the platform texture glue (T2.2/T2.3): it drives the Pigeon
// host API directly because the registry and the `WebmSticker` widget (T2.4,
// T2.5) don't exist yet. T2.7 replaces it with the real example pages.
// ignore_for_file: implementation_imports
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:webm_texture/src/gen/messages.g.dart';

/// A corpus file and the aspect ratio it is shown at.
class _Sticker {
  const _Sticker(this.asset, this.aspect);

  final String asset;

  /// Width / height of the source.
  final double aspect;
}

const _stickers = [
  _Sticker('vp9_alpha_512.webm', 1),
  _Sticker('vp8_alpha_256.webm', 1),
  _Sticker('vp9_alpha_vfr.webm', 1),
  _Sticker('vp9_odd_301x199.webm', 301 / 199),
  _Sticker('vp9_single_frame.webm', 1),
  _Sticker('vp9_opaque_256.webm', 1),
  _Sticker('vp9_with_audio.webm', 1),
  // Rejected by the core (WT_ERR_LIMIT): its tile shows the error code.
  _Sticker('vp9_2048.webm', 1),
];

/// Created once and disposed, so their status names land in the log.
const _rejected = ['garbage.bin', 'truncated.webm'];

const _tileWidth = 110.0;

class TextureDemoPage extends StatefulWidget {
  const TextureDemoPage({super.key});

  @override
  State<TextureDemoPage> createState() => _TextureDemoPageState();
}

class _Tile {
  _Tile(this.sticker);

  final _Sticker sticker;
  AnimationHandleMessage? handle;
  String? error;
}

class _TextureDemoPageState extends State<TextureDemoPage> {
  final _api = WebmTextureHostApi();
  final _tiles = [for (final s in _stickers) _Tile(s)];
  final _bytes = <String, Uint8List>{};
  final _log = <String>[];
  StreamSubscription<AnimationErrorMessage>? _errors;
  bool _playing = true;
  bool _started = false;
  bool _busy = false;
  int _cycled = 0;
  int _streamErrors = 0;
  double _dpr = 1;

  @override
  void initState() {
    super.initState();
    _errors = streamErrors().listen((e) {
      _streamErrors++;
      _say('anim ${e.animId}: ${e.code} ${e.message}');
    });
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    // Until the platform reports window metrics the view is 0 x 0 with a
    // device pixel ratio of 1; wait for real ones before picking buckets.
    if (!_started && !MediaQuery.sizeOf(context).isEmpty) {
      _started = true;
      _dpr = MediaQuery.devicePixelRatioOf(context);
      unawaited(_createAll());
    }
  }

  @override
  void dispose() {
    _errors?.cancel();
    for (final t in _tiles) {
      final h = t.handle;
      if (h != null) _api.dispose(h.animId);
    }
    super.dispose();
  }

  void _say(String line) {
    if (!mounted) return;
    setState(() => _log.insert(0, line));
  }

  Future<Uint8List> _load(String asset) async =>
      _bytes[asset] ??= (await rootBundle.load('test_assets/$asset')).buffer
          .asUint8List();

  Future<AnimationSourceMessage> _source(
    String name, {
    bool asFile = false,
  }) async {
    final bytes = await _load(name);
    if (!asFile) return AnimationSourceMessage(bytes: bytes);
    final file = File('${Directory.systemTemp.path}/wt_demo_$name');
    await file.writeAsBytes(bytes, flush: true);
    return AnimationSourceMessage(path: file.path);
  }

  /// The physical-pixel bucket for a tile, so the texture is drawn 1:1.
  (int, int) _bucket(_Sticker s) {
    final w = (_tileWidth * _dpr).round();
    return (w, (w / s.aspect).round());
  }

  Future<void> _createAll() async {
    for (final (i, t) in _tiles.indexed) {
      final s = t.sticker;
      try {
        // Alternate bytes and paths so both source kinds are exercised.
        final src = await _source(s.asset, asFile: i.isOdd);
        final (w, h) = _bucket(s);
        final handle = await _api.createAnimation(s.asset, src, w, h, true);
        if (!mounted) {
          await _api.dispose(handle.animId);
          return;
        }
        await _api.setActive(handle.animId, _playing);
        setState(() => t.handle = handle);
      } on PlatformException catch (e) {
        if (mounted) setState(() => t.error = e.code);
      }
    }
    for (final name in _rejected) {
      try {
        final handle = await _api.createAnimation(
          name,
          await _source(name),
          256,
          256,
          true,
        );
        _say('$name: created (texture ${handle.textureId})');
        await _api.dispose(handle.animId);
      } on PlatformException catch (e) {
        _say('$name: ${e.code}');
      }
    }
  }

  Future<void> _togglePlay() async {
    setState(() => _playing = !_playing);
    for (final t in _tiles) {
      final h = t.handle;
      if (h != null) await _api.setActive(h.animId, _playing);
    }
  }

  /// Creates, plays and disposes an animation [n] times, for leak checks
  /// (Instruments / `leaks`, or `dumpsys meminfo`).
  Future<void> _cycle(int n) async {
    setState(() => _busy = true);
    final sw = Stopwatch()..start();
    final s = _stickers.first;
    final src = await _source(s.asset);
    final (w, h) = _bucket(s);
    for (var i = 0; i < n && mounted; i++) {
      final handle = await _api.createAnimation('cycle', src, w, h, true);
      await _api.setActive(handle.animId, true);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      await _api.dispose(handle.animId);
      await _api.dispose(handle.animId); // idempotent
      if (mounted) setState(() => _cycled++);
    }
    _say('cycled $n in ${sw.elapsedMilliseconds} ms');
    if (mounted) setState(() => _busy = false);
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
            children: [for (final t in _tiles) _TileView(t)],
          ),
          const SizedBox(height: 12),
          Text(
            'live ${_tiles.where((t) => t.handle != null).length} · '
            'cycled $_cycled · errors $_streamErrors',
            key: const Key('status'),
          ),
          for (final line in _log)
            Text(line, style: const TextStyle(fontSize: 12)),
        ],
      ),
    );
  }
}

class _TileView extends StatelessWidget {
  const _TileView(this.tile);

  final _Tile tile;

  @override
  Widget build(BuildContext context) {
    final h = tile.handle;
    final Widget content;
    if (h != null) {
      content = Texture(
        textureId: h.textureId,
        filterQuality: FilterQuality.none,
      );
    } else {
      content = Center(
        child: Text(
          tile.error ?? '…',
          textAlign: TextAlign.center,
          style: const TextStyle(fontSize: 11, color: Colors.red),
        ),
      );
    }
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        CustomPaint(
          painter: const _Checker(),
          child: SizedBox(
            width: _tileWidth,
            height: _tileWidth / tile.sticker.aspect,
            child: content,
          ),
        ),
        SizedBox(
          width: _tileWidth,
          child: Text(
            tile.sticker.asset,
            overflow: TextOverflow.ellipsis,
            style: const TextStyle(fontSize: 10),
          ),
        ),
      ],
    );
  }
}

/// A checkerboard behind each texture, so transparency is visible.
class _Checker extends CustomPainter {
  const _Checker();

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
          canvas.drawRect(
            Rect.fromLTWH(
              x * cell,
              y * cell,
              cell,
              cell,
            ).intersect(Offset.zero & size),
            dark,
          );
        }
      }
    }
  }

  @override
  bool shouldRepaint(_Checker oldDelegate) => false;
}
