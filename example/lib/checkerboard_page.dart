// Checkerboard: alpha correctness. Premultiplied stickers over a checker
// pattern should show it through their transparent parts, with no dark or
// light fringes on the edges.
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus.dart';

/// Stickers shown, one per grid cell. The opaque one is the control.
const checkerboardStickers = [
  'vp9_alpha_512.webm',
  'vp8_alpha_256.webm',
  'vp9_alpha_vfr.webm',
  'vp9_odd_301x199.webm',
  'vp9_single_frame.webm',
  'vp9_opaque_256.webm',
];

class CheckerboardPage extends StatefulWidget {
  const CheckerboardPage({super.key});

  @override
  State<CheckerboardPage> createState() => _CheckerboardPageState();
}

class _CheckerboardPageState extends State<CheckerboardPage> {
  final _controller = WebmStickerController();
  bool _dark = false;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(
      title: const Text('Checkerboard'),
      actions: [
        IconButton(
          key: const Key('toggleBackground'),
          tooltip: 'Dark or light checks',
          icon: Icon(_dark ? Icons.light_mode : Icons.dark_mode),
          onPressed: () => setState(() => _dark = !_dark),
        ),
        ListenableBuilder(
          listenable: _controller,
          builder: (_, _) => IconButton(
            key: const Key('play'),
            tooltip: _controller.isPlaying ? 'Pause' : 'Play',
            icon: Icon(_controller.isPlaying ? Icons.pause : Icons.play_arrow),
            onPressed: _controller.isPlaying
                ? _controller.pause
                : _controller.play,
          ),
        ),
      ],
    ),
    body: CustomPaint(
      key: const Key('checkerboard'),
      painter: _CheckerPainter(
        a: _dark ? const Color(0xFF202020) : const Color(0xFFFFFFFF),
        b: _dark ? const Color(0xFF404040) : const Color(0xFFCCCCCC),
      ),
      child: GridView.count(
        crossAxisCount: 2,
        padding: const EdgeInsets.all(12),
        mainAxisSpacing: 12,
        crossAxisSpacing: 12,
        children: [
          for (final name in checkerboardStickers)
            WebmSticker(
              source: corpusSource(name),
              controller: _controller,
              errorBuilder: (_, e) => Center(child: Text('$e')),
            ),
        ],
      ),
    ),
  );
}

class _CheckerPainter extends CustomPainter {
  const _CheckerPainter({required this.a, required this.b});

  final Color a;
  final Color b;

  static const _cell = 16.0;

  @override
  void paint(Canvas canvas, Size size) {
    final paint = Paint()..color = a;
    canvas.drawRect(Offset.zero & size, paint);
    paint.color = b;
    for (var y = 0; y * _cell < size.height; y++) {
      for (var x = y.isEven ? 0 : 1; x * _cell < size.width; x += 2) {
        canvas.drawRect(
          Rect.fromLTWH(x * _cell, y * _cell, _cell, _cell),
          paint,
        );
      }
    }
  }

  @override
  bool shouldRepaint(_CheckerPainter old) => old.a != a || old.b != b;
}
