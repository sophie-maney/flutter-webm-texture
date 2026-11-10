// Demo of the platform texture glue (T2.2): stickers over a checkerboard, a
// play/pause toggle, and a create/dispose churn for leak checks.
//
// It talks to the generated Pigeon API directly because the public widget
// (`WebmSticker`, T2.5) and registry (T2.4) don't exist yet; T2.7 rebuilds
// this app on top of them.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
// ignore: implementation_imports
import 'package:webm_texture/src/gen/messages.g.dart';

void main() {
  runApp(const MaterialApp(title: 'webm_texture', home: DemoPage()));
}

/// A corpus file and the logical size it is shown at.
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
  // Rejected by the core (WT_ERR_LIMIT): shows the error path.
  _Sticker('vp9_2048.webm', 1),
];

const _tileWidth = 110.0;

class DemoPage extends StatefulWidget {
  const DemoPage({super.key});

  @override
  State<DemoPage> createState() => _DemoPageState();
}

class _Tile {
  _Tile(this.sticker);

  final _Sticker sticker;
  AnimationHandleMessage? handle;
  String? error;
}

class _DemoPageState extends State<DemoPage> {
  final _api = WebmTextureHostApi();
  final _tiles = [for (final s in _stickers) _Tile(s)];
  final _bytes = <String, Uint8List>{};
  final _events = <String>[];
  StreamSubscription<AnimationErrorMessage>? _errors;
  bool _playing = true;
  bool _started = false;
  int _churned = 0;
  double _dpr = 1;
  bool _churning = false;

  @override
  void initState() {
    super.initState();
    _errors = streamErrors().listen((e) {
      setState(() => _events.add('anim ${e.animId}: ${e.code} ${e.message}'));
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
      _createAll();
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

  Future<Uint8List> _load(String asset) async =>
      _bytes[asset] ??= (await rootBundle.load('test_assets/$asset')).buffer
          .asUint8List();

  /// The physical-pixel bucket for a tile, so the texture is drawn 1:1.
  (int, int) _bucket(_Sticker s) {
    final w = (_tileWidth * _dpr).round();
    return (w, (w / s.aspect).round());
  }

  Future<AnimationHandleMessage> _create(_Sticker s) async {
    final (w, h) = _bucket(s);
    final bytes = await _load(s.asset);
    return _api.createAnimation(
      s.asset,
      AnimationSourceMessage(bytes: bytes),
      w,
      h,
    );
  }

  Future<void> _createAll() async {
    for (final t in _tiles) {
      try {
        final h = await _create(t.sticker);
        if (!mounted) {
          await _api.dispose(h.animId);
          return;
        }
        await _api.setActive(h.animId, _playing);
        setState(() => t.handle = h);
      } on PlatformException catch (e) {
        if (mounted) setState(() => t.error = e.code);
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

  /// Creates, plays and disposes an animation 100 times, for meminfo checks.
  Future<void> _churn() async {
    setState(() => _churning = true);
    for (var i = 0; i < 100 && mounted; i++) {
      final h = await _create(_stickers.first);
      await _api.setActive(h.animId, true);
      await Future<void>.delayed(const Duration(milliseconds: 20));
      await _api.dispose(h.animId);
      if (mounted) setState(() => _churned++);
    }
    if (mounted) setState(() => _churning = false);
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('webm_texture')),
      body: ListView(
        padding: const EdgeInsets.all(12),
        children: [
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [
              FilledButton(
                key: const Key('play'),
                onPressed: _togglePlay,
                child: Text(_playing ? 'Pause' : 'Play'),
              ),
              OutlinedButton(
                key: const Key('churn'),
                onPressed: _churning ? null : _churn,
                child: const Text('Churn ×100'),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Text(
            'live ${_tiles.where((t) => t.handle != null).length} · '
            'churned $_churned${_churning ? '…' : ''} · '
            'errors ${_events.length}',
            key: const Key('status'),
          ),
          const SizedBox(height: 12),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            children: [for (final t in _tiles) _TileView(t)],
          ),
          for (final e in _events)
            Text(e, style: const TextStyle(fontSize: 12)),
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
          painter: const _Checkerboard(),
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

/// Grey/white squares behind each sticker, so transparency is visible.
class _Checkerboard extends CustomPainter {
  const _Checkerboard();

  @override
  void paint(Canvas canvas, Size size) {
    const cell = 10.0;
    final dark = Paint()..color = const Color(0xFFBDBDBD);
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.white);
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
  bool shouldRepaint(_Checkerboard oldDelegate) => false;
}
