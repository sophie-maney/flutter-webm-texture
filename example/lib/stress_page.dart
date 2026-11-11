// Stress: a 200-item grid of stickers. In atlas mode (the default for grids,
// ADR 0006) the grid sits in a StickerAtlasScope and the stickers on screen
// share a few atlas page textures; the layers button switches to standalone,
// one texture per animation. The "own" switch gives every cell its own
// animation through a controller of its own; without it, cells showing the
// same file share one. The grid button makes the cells small enough for 60+
// on a phone screen, the T3.3 budget (integration_test/sched_perf_test.dart,
// atlas_perf_test.dart).
import 'package:flutter/material.dart';
import 'package:webm_texture/webm_texture.dart';

import 'corpus.dart';

/// Number of cells.
const stressItemCount = 200;

class StressPage extends StatefulWidget {
  const StressPage({
    super.key,
    this.initiallyDense = false,
    this.initiallyPrivate = false,
    this.initiallyAtlas = true,
    this.columns,
    this.rows,
  });

  /// Starts in atlas mode (a StickerAtlasScope around the grid).
  final bool initiallyAtlas;

  /// Starts with the small cells.
  final bool initiallyDense;

  /// Starts with an animation per cell.
  final bool initiallyPrivate;

  /// A fixed number of columns, with the small cells' spacing; overrides the
  /// grid button. Tests use it to put a known number of cells on screen.
  final int? columns;

  /// With [columns]: stretches the cells so exactly this many rows show.
  final int? rows;

  @override
  State<StressPage> createState() => _StressPageState();
}

class _StressPageState extends State<StressPage> {
  List<WebmStickerController>? _controllers;
  late bool _dense = widget.initiallyDense;
  late bool _atlas = widget.initiallyAtlas;

  @override
  void initState() {
    super.initState();
    if (widget.initiallyPrivate) _controllers = _newControllers();
  }

  static List<WebmStickerController> _newControllers() => [
    for (var i = 0; i < stressItemCount; i++) WebmStickerController(),
  ];

  bool get _private => _controllers != null;

  /// Cell width / height so that [StressPage.rows] rows show in
  /// [constraints]: row `rows` starts just past the bottom edge, row
  /// `rows - 1` ends just inside it. Square without [StressPage.rows].
  double _aspect(BoxConstraints constraints) {
    final columns = widget.columns!;
    final rows = widget.rows;
    if (rows == null || !constraints.hasBoundedHeight) return 1;
    final width = (constraints.maxWidth - 16 - (columns - 1) * 4) / columns;
    // 8 px of padding above the first row.
    final pitch = (constraints.maxHeight - 8) / rows + 0.5;
    return width / (pitch - 4);
  }

  void _setPrivate(bool value) {
    final old = _controllers;
    setState(() {
      _controllers = value ? _newControllers() : null;
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
          IconButton(
            key: const Key('atlasMode'),
            tooltip: _atlas ? 'Standalone textures' : 'Atlas',
            icon: Icon(_atlas ? Icons.layers : Icons.layers_clear),
            onPressed: () => setState(() => _atlas = !_atlas),
          ),
          IconButton(
            key: const Key('denseGrid'),
            tooltip: _dense ? 'Large cells' : 'Small cells',
            icon: Icon(_dense ? Icons.grid_view : Icons.apps),
            onPressed: () => setState(() => _dense = !_dense),
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
      body: LayoutBuilder(
        builder: (context, constraints) => _maybeAtlas(
          GridView.builder(
            key: const Key('stressGrid'),
            padding: const EdgeInsets.all(8),
            gridDelegate: widget.columns != null
                ? SliverGridDelegateWithFixedCrossAxisCount(
                    crossAxisCount: widget.columns!,
                    mainAxisSpacing: 4,
                    crossAxisSpacing: 4,
                    childAspectRatio: _aspect(constraints),
                  )
                : SliverGridDelegateWithMaxCrossAxisExtent(
                    maxCrossAxisExtent: _dense ? 56 : 96,
                    mainAxisSpacing: _dense ? 4 : 8,
                    crossAxisSpacing: _dense ? 4 : 8,
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
        ),
      ),
    );
  }

  Widget _maybeAtlas(Widget grid) => _atlas
      ? StickerAtlasScope(key: const Key('stressAtlas'), child: grid)
      : grid;
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
