// Stress: a 200-item grid of standalone stickers (one texture per animation).
// The switch gives every cell its own animation through a controller of its
// own; without it, cells showing the same file share one.
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus.dart';

/// Number of cells.
const stressItemCount = 200;

class StressPage extends StatefulWidget {
  const StressPage({super.key});

  @override
  State<StressPage> createState() => _StressPageState();
}

class _StressPageState extends State<StressPage> {
  List<WebmStickerController>? _controllers;

  bool get _private => _controllers != null;

  void _setPrivate(bool value) {
    final old = _controllers;
    setState(() {
      _controllers = value
          ? [for (var i = 0; i < stressItemCount; i++) WebmStickerController()]
          : null;
    });
    // Dispose after the grid stopped listening.
    if (old != null) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        for (final c in old) {
          c.dispose();
        }
      });
    }
  }

  @override
  void dispose() {
    for (final c in _controllers ?? const <WebmStickerController>[]) {
      c.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final controllers = _controllers;
    return Scaffold(
      appBar: AppBar(
        title: const Text('Stress'),
        actions: [
          const Padding(
            padding: EdgeInsets.only(right: 8),
            child: Center(child: _ActiveCounter()),
          ),
          Switch(
            key: const Key('privateAnimations'),
            value: _private,
            onChanged: _setPrivate,
          ),
          const Padding(
            padding: EdgeInsets.only(right: 12),
            child: Center(child: Text('own')),
          ),
        ],
      ),
      body: GridView.builder(
        key: const Key('stressGrid'),
        padding: const EdgeInsets.all(8),
        gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
          maxCrossAxisExtent: 96,
          mainAxisSpacing: 8,
          crossAxisSpacing: 8,
        ),
        itemCount: stressItemCount,
        itemBuilder: (context, i) => WebmSticker(
          source: corpusSource(playableCorpus[i % playableCorpus.length]),
          controller: controllers?[i],
          placeholder: const ColoredBox(color: Color(0x14000000)),
          errorBuilder: (_, e) => Center(
            child: Text(
              e is StickerException ? e.name : '$e',
              style: const TextStyle(fontSize: 9, color: Colors.red),
            ),
          ),
        ),
      ),
    );
  }
}

/// Shows [WebmStickers.debugActiveCount].
class _ActiveCounter extends StatelessWidget {
  const _ActiveCounter();

  @override
  Widget build(BuildContext context) => ValueListenableBuilder<int>(
    valueListenable: WebmStickers.debugActiveCount,
    builder: (_, n, _) => Text('active $n', key: const Key('activeCount')),
  );
}
